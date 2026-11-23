#include "room_xschem.h"
#include "xschem_io.h"
#include "serialization.h"
#include "layer_utils.h"
#include <capnp/serialize.h>
#include <capnp/dynamic.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <tuple>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

using room::xschem::Record;
struct rx_error { std::string message; };
struct rx_document {
    std::string source, path, cell;
    std::vector<unsigned char> bytes;
    bool staging = false;
    size_t ci = 0, vi = 0;
    std::vector<Record> staged, baseline;
    std::map<std::string, size_t> identities;
};

namespace {
rx_error oom{"ROOM: cannot allocate error details"};

// Report context without allowing allocation failures to escape the C boundary.
void report(rx_error **out, const rx_document *doc, const char *message) noexcept
{
    if (!out) return;
    try { *out = new rx_error{(doc ? doc->path + " [" + doc->cell + "]" : "ROOM") + ": " + message}; }
    catch (...) { *out = &oom; }
}

template<class F> int boundary(rx_document *doc, rx_error **error, F &&fn) noexcept
{
    if (error) *error = nullptr;
    try { fn(); return 1; }
    catch (const kj::Exception &e) { report(error, doc, e.getDescription().cStr()); }
    catch (const std::exception &e) { report(error, doc, e.what()); }
    catch (...) { report(error, doc, "unknown C++ exception"); }
    return 0;
}

std::string string(rx_string value)
{
    if (!value.data && value.size) throw std::runtime_error("Null counted string");
    std::string s(value.data ? value.data : "", value.size);
    if (s.find('\0') != std::string::npos) throw std::runtime_error("Embedded NUL in string");
    return s;
}
rx_string borrowed(const std::string &s) { return {s.data(), s.size()}; }

std::vector<unsigned char> readBytes(const std::string &path)
{
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Expected a regular ROOM file: " + path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0) throw std::runtime_error("Cannot read nonempty file: " + path);
    std::vector<unsigned char> bytes(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) throw std::runtime_error("Read failed: " + path);
    return bytes;
}

// Keep Cap'n Proto readers scoped to the bytes they inspect.
template<class F> auto decode(const std::vector<unsigned char> &bytes, F &&fn)
{
    kj::ArrayInputStream input(kj::arrayPtr(bytes.data(), bytes.size()));
    capnp::ReaderOptions options;
    options.traversalLimitInWords = bytes.size() / 8 * 8 + 1024 * 1024;
    capnp::InputStreamMessageReader reader(input, options);
    return fn(reader.getRoot<room::schema::Database>());
}

void require(rx_document *doc)
{
    if (!doc) throw std::runtime_error("No open ROOM document");
}

void validate(const Record &r)
{
    size_t n;
    switch (r.kind) {
    case 'v': case 'G': case 'K': case 'V': case 'S': case 'E': case 'F': n = 0; break;
    case 'C': n = 2; break;
    case 'T': case 'N': case 'L': case 'B': n = 4; break;
    case 'A': n = 5; break;
    case 'P': if (r.values.size() < 4 || r.values.size() % 2) throw std::runtime_error("Invalid polygon points"); n = r.values.size(); break;
    default: throw std::runtime_error("Unsupported record kind");
    }
    if (r.text.find('\0') != std::string::npos || r.attributes.find('\0') != std::string::npos)
        throw std::runtime_error("Embedded NUL in string");
    if (r.text.find('\r') != std::string::npos || r.attributes.find('\r') != std::string::npos)
        throw std::runtime_error("Carriage returns are unsupported: native Xschem cannot reload them exactly");
    if (n != r.values.size()) throw std::runtime_error("Wrong coordinate count");
    for (double v : r.values) if (!std::isfinite(v) || std::abs(v) > 1e12) throw std::runtime_error("Invalid or oversized coordinate");
    if (r.layer < 0 || r.layer > 32767 || r.rotation < 0 || r.rotation > 3 || r.flip < 0 || r.flip > 1)
        throw std::runtime_error("Invalid layer or orientation");
}

// Refuse structures the flat exporter would omit or flatten silently.
void preflight(const room::Database &db, const room::CellContent &view)
{
    const auto &format = view.sourceInfo().format();
    if (!format.empty() && format != "xschem")
        throw std::runtime_error("Phase 1 supports Xschem-origin schematic views only: " + format);
    for (const auto &inst : view.block().instances()) {
        if (inst.transform().mag != 1.0) throw std::runtime_error("Instance magnification is unsupported");
        if (inst.columns() || inst.rows() || !inst.offsets().empty()) throw std::runtime_error("Instance arrays are unsupported");
        const auto stem = std::filesystem::path(inst.cellName()).stem().string();
        for (const auto &cell : db.lib().cells()) {
            if (cell.name() == inst.cellName() || cell.name() == stem) throw std::runtime_error("ROOM-local symbols/hierarchy are unsupported: " + inst.cellName());
        }
    }
    for (const auto &s : view.block().shapes()) {
        const auto &props = s.properties();
        if (std::any_of(props.begin(), props.end(), [](const auto &p) { return p.name.rfind("xschem.native.", 0) == 0; })) {
            auto need = [&](const char *key) {
                if (std::none_of(props.begin(), props.end(), [&](const auto &p) { return p.name == key; }))
                    throw std::runtime_error(std::string("Incomplete native metadata: missing ") + key);
            };
            need("xschem.native.attributes"); need("xschem.native.layer");
            if (s.text()) {
                need("xschem.native.sizeX"); need("xschem.native.sizeY");
                need("xschem.native.rotate"); need("xschem.native.flip");
            }
            std::set<std::string> keys;
            for (const auto &p : props) if (p.name.rfind("xschem.native.", 0) == 0) {
                if (!keys.insert(p.name).second) throw std::runtime_error("Duplicate native metadata: " + p.name);
                if (p.name == "xschem.native.layer" || p.name == "xschem.native.rotate" || p.name == "xschem.native.flip") {
                    size_t used = 0; const int value = std::stoi(p.value, &used);
                    const int max = p.name == "xschem.native.layer" ? 32767 : p.name == "xschem.native.rotate" ? 3 : 1;
                    if (used != p.value.size() || value < 0 || value > max) throw std::runtime_error("Invalid native metadata: " + p.name);
                }
                if (p.name == "xschem.native.sizeX" || p.name == "xschem.native.sizeY") {
                    size_t used = 0; const double value = std::stod(p.value, &used);
                    if (used != p.value.size() || !std::isfinite(value) || value <= 0) throw std::runtime_error("Invalid native metadata: " + p.name);
                }
            }
        }

        if (s.path() && (s.path()->bgnExt || s.path()->endExt || s.path()->round))
            throw std::runtime_error("Path cap/extension styles are unsupported");
        auto repeated = [](const auto &g) { return g.columns || g.rows || !g.copies.empty(); };
        if (s.rect() && (repeated(*s.rect()) || ((s.rect()->layerId < view.layers().size() ? view.layers()[s.rect()->layerId].purpose : room::LayerPurpose::Drawing) != room::LayerPurpose::Pin &&
                std::none_of(s.properties().begin(), s.properties().end(), [](const auto &p) { return p.name == "xschem.native.attributes"; }))))
            throw std::runtime_error("Non-pin ROOM rectangles or arrays are unsupported");
        if (s.polygon() && repeated(*s.polygon())) throw std::runtime_error("Polygon repetitions are unsupported");
        if (s.path() && (repeated(*s.path()) || (s.path()->points.size() != 2 &&
                std::none_of(s.properties().begin(), s.properties().end(), [](const auto &p) { return p.name == "geometry"; })))) throw std::runtime_error("Multi-point paths are unsupported");
    }
}

// Match the topology selection used by readViewTopology, including compact files.
template<class View> auto originalInstances(View view)
{
    auto block = view.getBlock();
    if (block.getShapes().size() || block.getInstances().size() || block.getNets().size()) return block.getInstances();
    return view.getCompact().getInstances();
}

// The scanner accepts only complete tokens, so a leading quote has a matching end.
std::string attributeValue(const std::string &spelling)
{
    if (spelling.front() != '"') return spelling;
    return room::xschem::decodeString(spelling.substr(1, spelling.size() - 2));
}

// Keep locked tokens in their original spelling.
std::map<std::string, std::string> attributes(const std::string &encoded, bool decodeValue = false)
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    const auto body = room::xschem::decodeString(encoded);
    std::map<std::string, std::string> result;
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && space(body[i])) ++i;
        if (i == body.size()) break;
        const auto start = i;
        while (i < body.size() && body[i] != '=' && !space(body[i])) ++i;
        if (i == start || i == body.size() || body[i] != '=') throw std::runtime_error("Malformed instance attributes");
        auto key = body.substr(start, i++ - start);
        if (key.find_first_of(";\\\"") != std::string::npos) throw std::runtime_error("Unsupported instance attribute name: " + key);
        const auto valueStart = i;
        if (i < body.size() && body[i] == '"') {
            ++i;
            while (i < body.size() && body[i] != '"') {
                if (body[i] == '\\') {
                    if (++i == body.size() || (body[i] != '\\' && body[i] != '"'))
                        throw std::runtime_error("Unsupported quoted attribute escape: " + key);
                }
                ++i;
            }
            if (i == body.size()) throw std::runtime_error("Unclosed attribute quote");
            ++i;
            if (i < body.size() && !space(body[i])) throw std::runtime_error("Malformed quoted attribute");
        } else {
            while (i < body.size() && !space(body[i])) {
                if (body[i] == '\\' || body[i] == '"' || body[i] == ';')
                    throw std::runtime_error("Unsupported unquoted attribute spelling: " + key);
                ++i;
            }
            if (i == valueStart) throw std::runtime_error("Empty attribute must be quoted: " + key);
        }
        auto value = body.substr(valueStart, i - valueStart);
        if (decodeValue && key == "value") value = attributeValue(value);
        if (!result.emplace(key, value).second) throw std::runtime_error("Duplicate instance attribute: " + key);
    }
    return result;
}

std::string identity(const Record &r)
{
    const auto props = attributes(r.attributes);
    const auto it = props.find("name");
    auto name = it == props.end() ? std::string{} : attributeValue(it->second);
    if (name.empty()) throw std::runtime_error("Every ROOM instance needs a unique nonempty name");
    return name;
}

// Xschem emits empty optional sections and orders wire endpoints on load.
std::vector<Record> drawing(std::vector<Record> records)
{
    std::vector<Record> result;
    for (auto r : records) {
        if (r.kind == 'C' || (std::string("GKVSEF").find(r.kind) != std::string::npos && r.attributes.empty())) continue;
        if (r.kind == 'v') while (!r.attributes.empty() && std::isspace(static_cast<unsigned char>(r.attributes.back()))) r.attributes.pop_back();
        if ((r.kind == 'N' || r.kind == 'L') && std::make_pair(r.values[0], r.values[1]) > std::make_pair(r.values[2], r.values[3])) {
            std::swap(r.values[0], r.values[2]); std::swap(r.values[1], r.values[3]);
        }
        if (r.kind == 'B') {
            if (r.values[0] > r.values[2]) std::swap(r.values[0], r.values[2]);
            if (r.values[1] > r.values[3]) std::swap(r.values[1], r.values[3]);
        }
        result.push_back(std::move(r));
    }
    std::sort(result.begin(), result.end(), [](const Record &a, const Record &b) {
        return std::tie(a.kind, a.layer, a.rotation, a.flip, a.text, a.values, a.attributes)
             < std::tie(b.kind, b.layer, b.rotation, b.flip, b.text, b.values, b.attributes);
    });
    return result;
}

bool equal(const Record &a, const Record &b, bool compareParameterValues = false)
{
    if (a.kind != b.kind || a.layer != b.layer || a.rotation != b.rotation || a.flip != b.flip ||
        a.text != b.text || a.values.size() != b.values.size()) return false;
    if (compareParameterValues && a.kind == 'C') {
        if (attributes(a.attributes, true) != attributes(b.attributes, true)) return false;
    } else if (a.attributes != b.attributes) return false;
    for (size_t i = 0; i < a.values.size(); ++i) if (a.values[i] != b.values[i]) return false;
    return true;
}

// Keep a persistent sibling inode: unlinking it would split waiting writers across locks.
struct WriterLock {
    int fd;
    explicit WriterLock(const std::string &target) {
        fd = open((target + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "Open writer lock");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (flock(fd, LOCK_EX | LOCK_NB)) {
            const int error = errno;
            if (error != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline) {
                close(fd);
                throw std::system_error(error, std::generic_category(), "Acquire writer lock");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ~WriterLock() { close(fd); }
};

// RAII retains the original on all pre-publication failures.
struct Temporary {
    std::string path;
    int fd = -1;
    explicit Temporary(const std::string &target) : path(target + ".xschem-XXXXXX") {
        fd = mkstemp(path.data());
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "Create sibling temporary");
    }
    ~Temporary() { if (fd >= 0) close(fd); if (!path.empty()) unlink(path.c_str()); }
};
// Detect external writes by comparing the exact opened revision.
void checkRevision(const rx_document &doc)
{
    if (std::filesystem::canonical(doc.source) != doc.path || readBytes(doc.path) != doc.bytes)
        throw std::runtime_error("Save conflict: ROOM file changed since open; editor edits retained");
}

template<class Root> auto viewInstances(Root root, const rx_document &doc)
{
    return originalInstances(root.getLib().getCells()[doc.ci].getContents()[doc.vi].getPayload().getSchematic());
}

// Load once and check the complete projection before supplying any records to the editor.
void readDrawing(rx_document &doc)
{
    const auto db = decode(doc.bytes, [](auto root) { return room::readDatabase(root); });
    const auto &cells = db.lib().cells();
    const auto filename = std::filesystem::path(doc.source).filename().string();
    const room::CellContent *view = nullptr;
    size_t count = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto *candidate = cells[i].findContent(room::ViewType::Schematic);
        if (!candidate) continue;
        if (++count == 1 || filename == cells[i].name() + ".schematic.room") {
            doc.ci = i;
            view = candidate;
        }
    }
    if (!view || (count != 1 && filename != cells[doc.ci].name() + ".schematic.room"))
        throw std::runtime_error("Expected one schematic or a filename of <cell>.schematic.room");
    const auto &cell = cells[doc.ci];
    doc.vi = view - cell.contents().data();
    preflight(db, *view);
    auto records = room::xschem::collectRecords(cell, *view, true);
    decode(doc.bytes, [&](auto root) {
        const auto instances = viewInstances(root, doc);
        for (size_t i = 0; i < instances.size(); ++i) {
            std::string name;
            size_t count = 0;
            for (auto p : instances[i].getProperties()) if (p.getName() == "name") {
                if (++count > 1) throw std::runtime_error("Duplicate ROOM name property");
                const auto value = p.getValue();
                name.assign(value.begin(), value.size());
            }
            if (name.empty() || !doc.identities.emplace(name, i).second)
                throw std::runtime_error("Every ROOM instance needs a unique nonempty name");
        }
    });
    std::set<std::string> delivered;
    for (const auto &r : records) {
        validate(r);
        if (r.kind != 'C') continue;
        const auto name = identity(r);
        if (!doc.identities.count(name) || !delivered.insert(name).second)
            throw std::runtime_error("Exported instance has no unique original ROOM identity: " + name);
    }
    if (delivered.size() != doc.identities.size()) throw std::runtime_error("Exporter omitted ROOM instances");
    doc.baseline = std::move(records);
    doc.cell = cell.name();
}
}

extern "C" {
int rx_open(rx_string path, rx_document **out, rx_error **error)
{
    if (out) *out = nullptr;
    return boundary(nullptr, error, [&] {
        if (!out) throw std::runtime_error("Null document output");
        auto doc = std::make_unique<rx_document>();
        doc->source = std::filesystem::absolute(string(path)).string();
        doc->path = std::filesystem::canonical(doc->source).string();
        doc->bytes = readBytes(doc->path);
        const auto first = std::find_if_not(doc->bytes.begin(), doc->bytes.end(), [](unsigned char c) { return std::isspace(c); });
        if (first != doc->bytes.end() && *first == '{')
            throw std::runtime_error("Native Xschem requires a binary ROOM document");
        *out = doc.release();
    });
}

int rx_read(rx_document *doc, rx_record_callback callback, void *user, rx_error **error)
{
    return boundary(doc, error, [&] {
        require(doc);
        if (!callback) throw std::runtime_error("Null record callback");
        if (doc->baseline.empty()) {
            doc->identities.clear();
            readDrawing(*doc);
        }
        for (const auto &r : doc->baseline) {
            rx_record c{r.kind, r.layer, r.rotation, r.flip, r.values.data(), r.values.size(), borrowed(r.text), borrowed(r.attributes)};
            if (!callback(user, &c)) throw std::runtime_error("Record callback refused data");
        }
    });
}

int rx_begin(rx_document *doc, rx_error **error)
{
    return boundary(doc, error, [&] {
        require(doc); if (doc->cell.empty()) throw std::runtime_error("Read a cell before staging");
        doc->staged.clear(); doc->staging = true;
    });
}
int rx_append(rx_document *doc, const rx_record *r, rx_error **error)
{
    const int ok = boundary(doc, error, [&] {
        require(doc); if (!doc->staging || !r) throw std::runtime_error("No staging operation");
        if ((!r->values && r->value_count) || r->value_count > 2000000) throw std::runtime_error("Invalid counted coordinates");
        Record v; v.kind = r->kind; v.layer = r->layer; v.rotation = r->rotation; v.flip = r->flip;
        if (r->value_count) v.values.assign(r->values, r->values + r->value_count);
        v.text = string(r->text); v.attributes = string(r->attributes); validate(v);
        doc->staged.push_back(std::move(v));
    });
    if (!ok && doc) doc->staging = false;
    return ok;
}

const char *rx_value_symbol_type(rx_string symbol)
{
    const std::string_view name(symbol.data ? symbol.data : "", symbol.data ? symbol.size : 0);
    if (name == "res.sym" || name == "devices/res.sym") return "resistor";
    if (name == "capa.sym" || name == "devices/capa.sym") return "capacitor";
    if (name == "ind.sym" || name == "devices/ind.sym") return "inductor";
    return nullptr;
}

int rx_commit(rx_document *doc, rx_error **error)
{
    return boundary(doc, error, [&] {
        require(doc);
        if (!doc->staging) throw std::runtime_error("No staged view");
        checkRevision(*doc);
        const char *refusal = "Only existing instance parameter values may change; editor edits retained";
        const auto beforeDrawing = drawing(doc->baseline), afterDrawing = drawing(doc->staged);
        if (beforeDrawing.size() != afterDrawing.size() ||
            !std::equal(beforeDrawing.begin(), beforeDrawing.end(), afterDrawing.begin(),
                [](const Record &a, const Record &b) { return equal(a, b); }))
            throw std::runtime_error(refusal);
        std::map<std::string, const Record *> edited;
        for (const auto &r : doc->staged) if (r.kind == 'C') {
            if (!edited.emplace(identity(r), &r).second) throw std::runtime_error(refusal);
        }
        if (edited.size() != doc->identities.size()) throw std::runtime_error(refusal);

        capnp::MallocMessageBuilder patched;
        decode(doc->bytes, [&](auto root) { patched.setRoot(root); });
        auto instances = viewInstances(patched.getRoot<room::schema::Database>(), *doc);
        struct Patch { size_t instance, property; std::string old; };
        std::vector<Patch> patches;
        auto expected = doc->baseline;
        for (auto &old : expected) if (old.kind == 'C') {
            const auto name = identity(old);
            const auto it = edited.find(name);
            if (it == edited.end()) throw std::runtime_error(refusal);
            auto geometry = *it->second;
            geometry.attributes = old.attributes;
            if (!equal(geometry, old)) throw std::runtime_error(refusal);
            const auto before = attributes(old.attributes, true), after = attributes(it->second->attributes, true);
            if (before.size() != after.size()) throw std::runtime_error(refusal);
            if (before == after) continue;
            const auto index = doc->identities.at(name);
            auto props = instances[index].getProperties();
            auto set = [&](const std::string &key, const std::string &value, bool required) {
                size_t found = props.size();
                for (size_t j = 0; j < props.size(); ++j) if (props[j].getName().asReader() == key.c_str()) {
                    if (found != props.size()) throw std::runtime_error("Ambiguous ROOM property: " + key);
                    found = j;
                }
                if (found != props.size()) {
                    const auto oldValue = props[found].getValue();
                    patches.push_back({index, found, std::string(oldValue.begin(), oldValue.size())});
                    props[found].setValue(value);
                } else if (required) throw std::runtime_error("Edited parameter has no original ROOM property: " + key);
            };
            for (const auto &[key, value] : before) {
                const auto next = after.find(key);
                if (next == after.end()) throw std::runtime_error(refusal);
                if (next->second == value) continue;
                if (key != "value" || !rx_value_symbol_type(borrowed(old.text)))
                    throw std::runtime_error("Instance attribute is read-only: " + name + "." + key);
                set(key, next->second, true);
            }
            set("xschem.native.attributes", it->second->attributes, false);
            old.attributes = it->second->attributes;
        }
        if (patches.empty()) { doc->staging = false; return; }
        WriterLock lock(doc->path);
        checkRevision(*doc);
        struct stat st;
        if (stat(doc->path.c_str(), &st)) throw std::system_error(errno, std::generic_category(), "Inspect ROOM file");
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || !(st.st_mode & 0222) || access(doc->path.c_str(), W_OK))
            throw std::runtime_error("ROOM save requires a writable regular file with one link");
        Temporary tmp(doc->path);
        if (fchmod(tmp.fd, st.st_mode & 0777)) throw std::system_error(errno, std::generic_category(), "Preserve permissions");
        capnp::writeMessageToFd(tmp.fd, patched);
        if (fsync(tmp.fd)) throw std::system_error(errno, std::generic_category(), "Flush temporary");
        if (close(std::exchange(tmp.fd, -1))) throw std::system_error(errno, std::generic_category(), "Close temporary");
        auto bytes = readBytes(tmp.path);
        const auto check = decode(bytes, [](auto root) { return room::readDatabase(root); });
        const auto &cell = check.lib().cells().at(doc->ci);
        const auto records = room::xschem::collectRecords(cell, cell.contents().at(doc->vi), true);
        if (records.size() != expected.size() ||
            !std::equal(records.begin(), records.end(), expected.begin(),
                [](const Record &a, const Record &b) { return equal(a, b, true); }))
            throw std::runtime_error("Temporary parameter validation failed");

        // Undo precisely the authorized edits in the readback before comparing the full raw message.
        capnp::MallocMessageBuilder masked;
        decode(bytes, [&](auto root) { masked.setRoot(root); });
        auto restored = viewInstances(masked.getRoot<room::schema::Database>(), *doc);
        for (const auto &patch : patches) {
            auto property = restored[patch.instance].getProperties()[patch.property];
            if (property.getValue().asReader() != instances[patch.instance].getProperties()[patch.property].getValue().asReader())
                throw std::runtime_error("Temporary property value differs from the requested edit");
            property.setValue(patch.old);
        }
        decode(doc->bytes, [&](auto original) {
            const auto before = capnp::canonicalize(original);
            const auto after = capnp::canonicalize(masked.getRoot<room::schema::Database>().asReader());
            if (before.size() != after.size() || std::memcmp(before.begin(), after.begin(), before.size() * sizeof(capnp::word)))
                throw std::runtime_error("Unedited ROOM data changed");
        });
        checkRevision(*doc);
        if (rename(tmp.path.c_str(), doc->path.c_str())) throw std::system_error(errno, std::generic_category(), "Atomic replace");
        tmp.path.clear();
        doc->bytes = std::move(bytes);
        doc->baseline = std::move(expected);
        doc->staging = false;
    });
}
void rx_close(rx_document *doc) { delete doc; }
rx_string rx_error_message(const rx_error *error) { return error ? borrowed(error->message) : rx_string{nullptr, 0}; }
void rx_error_free(rx_error *error) { if (error != &oom) delete error; }
}
