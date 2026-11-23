#include "room_xschem.h"
#include "database.h"
#include "serialization.h"
#include "xschem_importer.h"
#include "xschem_io.h"
#include <capnp/serialize.h>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using room::xschem::Record;
namespace fs = std::filesystem;

void require(bool value, const std::string &message)
{
    if (!value) throw std::runtime_error(message);
}
void checked(int ok, rx_error *error)
{
    auto msg = rx_error_message(error);
    std::string text(msg.data ? msg.data : "", msg.size);
    rx_error_free(error);
    require(ok, text);
}
rx_string str(const std::string &s) { return {s.data(), s.size()}; }
struct Document {
    rx_document *doc = nullptr;
    explicit Document(const fs::path &path) {
        rx_error *e = nullptr; const auto s = path.string(); int ok = rx_open(str(s), &doc, &e); checked(ok, e);
    }
    ~Document() { rx_close(doc); }
};
int collect(void *out, const rx_record *r)
{
    Record v; v.kind = r->kind; v.layer = r->layer; v.rotation = r->rotation; v.flip = r->flip;
    if (r->value_count) v.values.assign(r->values, r->values + r->value_count);
    v.text.assign(r->text.data, r->text.size); v.attributes.assign(r->attributes.data, r->attributes.size);
    static_cast<std::vector<Record> *>(out)->push_back(std::move(v)); return 1;
}
std::vector<Record> read(Document &doc)
{
    std::vector<Record> result; rx_error *e = nullptr;
    int ok = rx_read(doc.doc, collect, &result, &e); checked(ok, e); return result;
}
void stage(Document &doc, const std::vector<Record> &records, bool verifyDevices = true)
{
    rx_error *e = nullptr; int ok = rx_begin(doc.doc, &e); checked(ok, e);
    for (const auto &v : records) {
        rx_record r{v.kind, v.layer, v.rotation, v.flip, v.values.data(), v.values.size(), str(v.text), str(v.attributes)};
        // Simulate editor verification. Loaded symbol identity is covered by the editor regression.
        ok = verifyDevices && v.kind == 'C' && rx_value_symbol_type(r.text)
           ? rx_append_verified_instance(doc.doc, &r, &e)
           : rx_append(doc.doc, &r, &e);
        checked(ok, e);
    }
}
std::string bytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary); require(bool(in), "Read fixture bytes");
    std::ostringstream out; out << in.rdbuf(); return out.str();
}
std::string formatted(const std::vector<Record> &records)
{
    std::ostringstream out;
    for (const auto &r : records) room::xschem::formatRecord(out, r);
    return out.str();
}

// Oracle: compare typed C objects with the existing exporter parsed back into values.
void oracle(const fs::path &source, const fs::path &target)
{
    room::XschemImporter importer;
    auto db = importer.importFile(source.string());
    require(importer.errors().empty(), "Cannot import " + source.string());
    require(db.lib().cells().size() == 1, "Expected one imported cell");
    db.saveToFile(target.string(), room::ViewType::Schematic);
    auto persisted = room::Database::loadFromFile(target.string());
    const auto &cell = persisted.lib().cells().front();
    const auto *view = cell.findContent(room::ViewType::Schematic);
    require(view, "Expected schematic view");
    const auto expected = room::xschem::collectRecords(cell, *view, true);
    std::ostringstream out;
    for (const auto &record : expected) room::xschem::formatRecord(out, record);
    std::vector<std::string> errors;
    std::vector<Record> parsed;
    for (const auto &line : room::xschem::readRecordsFromText(out.str(), errors)) parsed.push_back(room::xschem::parseRecord(line));
    require(errors.empty(), "Exporter output could not be parsed");
    Document doc(target); auto direct = read(doc);
    require(formatted(direct) == formatted(parsed), "Typed/export oracle mismatch for " + source.string());
    std::cout << "PASS direct/export oracle: " << source.filename() << " (" << direct.size() << " records)\n";
}

// Save a selected schematic while checking unrelated layout, cells and future root fields.
void saves(const fs::path &target, bool compact)
{
    room::Database db;
    auto &cell = db.lib().getOrCreateCell("flat");
    auto &sch = cell.getOrCreateContent(room::ViewType::Schematic);
    sch.setDbuPerEditorUnit(1000);
    sch.properties().push_back({"editor.xschem.G", "initial=1"});
    room::Instance inst("devices/res.sym", {});
    inst.properties() = {{"name", "R1"}, {"value", "1k"}, {"xschem.native.attributes", "name=R1 value=1k"}, {"vendor.hidden", "KEEP-INSTANCE"}};
    sch.block().instances().push_back(inst);
    room::Instance second("devices/res.sym", {30000,40000});
    second.properties() = {{"name", "R2"}, {"value", "5k"}, {"xschem.native.attributes", "name=R2 value=5k"}};
    sch.block().instances().push_back(second);
    room::Shape::TextData text; text.text = "flat view"; text.position = {0,10000}; text.layerId = 1;
    room::Shape shape(text);
    shape.properties() = {{"xschem.native.attributes", ""}, {"xschem.native.layer", "0"},
        {"xschem.native.rotate", "0"}, {"xschem.native.flip", "0"}, {"xschem.native.sizeX", "0.4"}, {"xschem.native.sizeY", "0.6"}, {"vendor.opaque", "MUST-KEEP"}};
    sch.block().shapes().push_back(shape);
    room::Shape::PathData wire; wire.points = {{0,0}, {20000,0}};
    room::Shape wireShape(wire);
    wireShape.properties() = {{"xschem.native.attributes", ""}, {"xschem.native.layer", "0"}};
    sch.layers().emplace_back(0, 0, "wire", room::LayerPurpose::Wire);
    sch.layers().emplace_back(1, 0, "drawing");
    sch.block().shapes().push_back(wireShape);
    room::Net clock("clock", room::SigType::Clock);
    clock.terms().emplace_back("CLK", 0, room::Point{42,43});
    sch.block().nets().push_back(clock);
    auto &layout = cell.getOrCreateContent(room::ViewType::Layout);
    room::Shape::RectData rectangle; rectangle.box = {1,2,300,400}; layout.block().shapes().emplace_back(rectangle);
    auto &other = db.lib().getOrCreateCell("untouched");
    other.properties().push_back({"opaque metadata", "preserve this"});
    db.saveToFile(target.string(), room::ViewType::Schematic, {compact});
    // Enlarge the root with an unknown pointer field. Generated readers cannot see it.
    auto original = bytes(target);
    kj::ArrayInputStream input(kj::arrayPtr(reinterpret_cast<const kj::byte *>(original.data()), original.size()));
    capnp::InputStreamMessageReader reader(input);
    capnp::MallocMessageBuilder future;
    auto raw = future.initRoot<capnp::AnyPointer>().initAsAnyStruct(1, 8);
    auto known = reader.getRoot<room::schema::Database>();
    auto knownRaw = capnp::toAny(known);
    auto pointers = raw.getPointerSection();
    for (unsigned i = 0; i < knownRaw.getPointerSection().size(); ++i) pointers[i].set(knownRaw.getPointerSection()[i]);
    pointers[7].setAs<capnp::Text>("unknown future metadata");
    auto futureBytes = capnp::messageToFlatArray(future);
    { std::ofstream out(target, std::ios::binary); auto b = futureBytes.asBytes(); out.write(reinterpret_cast<const char *>(b.begin()), b.size()); }
    Document doc(target); auto records = read(doc);
    for (auto &r : records) if (r.kind == 'C' && r.attributes.find("name=R1 ") == 0) r.attributes = "name=R1 value=2k";
    auto reordered = records; std::reverse(reordered.begin(), reordered.end());
    stage(doc, reordered);
    rx_error *e = nullptr; int ok = rx_commit(doc.doc, &e); checked(ok, e);
    auto after = room::Database::loadFromFile(target.string());
    require(after.lib().cells()[0].findContent(room::ViewType::Layout)->block().shapes()[0].rect()->box.urx == 300, "Layout changed");
    require(after.lib().cells()[1].properties()[0].value == "preserve this", "Other cell changed");
    const auto savedBytes = bytes(target);
    kj::ArrayInputStream savedInput(kj::arrayPtr(reinterpret_cast<const kj::byte *>(savedBytes.data()), savedBytes.size()));
    capnp::InputStreamMessageReader saved(savedInput);
    auto savedRaw = capnp::toAny(saved.getRoot<room::schema::Database>());
    require(savedRaw.getPointerSection()[7].getAs<capnp::Text>() == "unknown future metadata", "Unknown field lost");
    auto expected = future.getRoot<room::schema::Database>();
    auto expectedView = expected.getLib().getCells()[0].getContents()[0].getPayload().getSchematic();
    auto expectedInstances = compact ? expectedView.getCompact().getInstances() : expectedView.getBlock().getInstances();
    for (auto p : expectedInstances[0].getProperties()) {
        if (p.getName().asReader() == "value") p.setValue("2k");
        if (p.getName().asReader() == "xschem.native.attributes") p.setValue("name=R1 value=2k");
    }
    auto expectedBytes = capnp::canonicalize(expected.asReader());
    auto actualBytes = capnp::canonicalize(saved.getRoot<room::schema::Database>());
    require(expectedBytes.size() == actualBytes.size() && !std::memcmp(expectedBytes.begin(), actualBytes.begin(), expectedBytes.size() * sizeof(capnp::word)),
        "Whole decoded message differs beyond the two allowed attribute values");
    auto originalLayout = capnp::canonicalize(known.getLib().getCells()[0].getContents()[1]);
    auto savedLayout = capnp::canonicalize(saved.getRoot<room::schema::Database>().getLib().getCells()[0].getContents()[1]);
    require(originalLayout.size() == savedLayout.size() && !std::memcmp(originalLayout.begin(), savedLayout.begin(), originalLayout.size() * sizeof(capnp::word)), "Untouched layout region differs");
    const auto &savedSch = *after.lib().cells()[0].findContent(room::ViewType::Schematic);
    require(savedSch.block().nets()[0].name() == "clock" && savedSch.block().nets()[0].terms()[0].position().x == 42, "Explicit net/terminal lost");
    std::cout << "PASS whole decoded message and untouched canonical layout bytes; opaque properties and explicit nets preserved (compact=" << compact << ")\n";
    Document reopened(target);
    require(formatted(read(reopened)) == formatted(records), "Edited record did not survive reopen");
    std::cout << "PASS stable identity across reordered objects; save/reopen preserves exact edited records\n";
    // Future fields inside the selected payload survive parameter saves too.
    capnp::MallocMessageBuilder unknownPayload, patched;
    auto extended = unknownPayload.initRoot<capnp::AnyPointer>().initAsAnyStruct(1, 5);
    auto schematic = capnp::toAny(saved.getRoot<room::schema::Database>().getLib().getCells()[0].getContents()[0].getPayload().getSchematic());
    for (unsigned i = 0; i < schematic.getPointerSection().size(); ++i)
        extended.getPointerSection()[i].set(schematic.getPointerSection()[i]);
    extended.getPointerSection()[4].setAs<capnp::Text>("future schematic data");
    patched.setRoot(saved.getRoot<room::schema::Database>());
    auto payload = patched.getRoot<room::schema::Database>().getLib().getCells()[0].getContents()[0].getPayload();
    capnp::toAny(payload).getPointerSection()[0].set(unknownPayload.getRoot<capnp::AnyPointer>());
    auto unknownBytes = capnp::messageToFlatArray(patched);
    const auto unknownPath = target.parent_path() / "unknown-edited.room";
    { std::ofstream out(unknownPath, std::ios::binary); const auto b = unknownBytes.asBytes(); out.write(reinterpret_cast<const char *>(b.begin()), b.size()); }
    Document unknown(unknownPath);
    auto futureRecords = read(unknown);
    for (auto &r : futureRecords) if (r.kind == 'C' && r.attributes.find("name=R1 ") == 0) r.attributes = "name=R1 value=3k";
    stage(unknown, futureRecords); e = nullptr;
    ok = rx_commit(unknown.doc, &e); checked(ok, e);
    const auto preserved = bytes(unknownPath);
    kj::ArrayInputStream futureInput(kj::arrayPtr(reinterpret_cast<const kj::byte *>(preserved.data()), preserved.size()));
    capnp::InputStreamMessageReader futureReader(futureInput);
    auto futureSch = capnp::toAny(futureReader.getRoot<room::schema::Database>().getLib().getCells()[0].getContents()[0].getPayload().getSchematic());
    require(futureSch.getPointerSection()[4].getAs<capnp::Text>() == "future schematic data", "Future selected payload lost");
    std::cout << "PASS unknown selected-payload data preserved through parameter save\n";
    // Every prohibited state change must fail without changing even one original byte.
    const auto baseline = read(reopened);
    auto refuse = [&](std::vector<Record> edited, const std::string &label, bool verifyDevices = true) {
        const auto originalBytes = bytes(target);
        stage(reopened, edited, verifyDevices); rx_error *error = nullptr;
        const int result = rx_commit(reopened.doc, &error);
        require(!result, "Accepted forbidden edit: " + label);
        auto msg = rx_error_message(error);
        require(msg.size > 0, "Missing refusal diagnostic"); rx_error_free(error);
        require(bytes(target) == originalBytes, "Forbidden edit wrote file: " + label);
        std::cout << "PASS refuse " << label << "; original byte-identical\n";
    };
    auto mutate = [&](const std::string &label, auto change) {
        auto edited = baseline; change(edited); refuse(edited, label);
    };
    auto instance = [](auto &records) -> Record & { return *std::find_if(records.begin(), records.end(), [](const auto &r) { return r.kind == 'C'; }); };
    auto unverified = baseline;
    auto &unverifiedDevice = instance(unverified);
    unverifiedDevice.attributes = "name=R1 value=123456";
    refuse(std::move(unverified), "value on an unverified symbol", false);
    mutate("added instance", [&](auto &r) { auto v = instance(r); v.attributes = "name=R2 value=2k"; r.push_back(v); });
    mutate("deleted instance", [&](auto &r) { r.erase(std::find_if(r.begin(), r.end(), [](const auto &v) { return v.kind == 'C'; })); });
    mutate("tiny move", [&](auto &r) { for (auto &v : r) if (v.kind == 'C') v.values[0] += 1e-10; });
    mutate("moved instance", [&](auto &r) { instance(r).values[0] += 10; });
    mutate("rotated instance", [&](auto &r) { instance(r).rotation = 1; });
    mutate("flipped instance", [&](auto &r) { instance(r).flip = 1; });
    mutate("renamed instance", [&](auto &r) { instance(r).attributes = "name=R2 value=2k"; });
    mutate("changed symbol", [&](auto &r) { instance(r).text = "devices/capa.sym"; });
    mutate("changed text", [&](auto &r) { for (auto &v : r) if (v.kind == 'T') v.text += "changed"; });
    mutate("changed wire", [&](auto &r) { for (auto &v : r) if (v.kind == 'N') v.values[0] += 10; });
    mutate("added object", [&](auto &r) { Record v; v.kind = 'L'; v.values = {1,2,3,4}; r.push_back(v); });
    mutate("deleted object", [&](auto &r) { r.erase(std::find_if(r.begin(), r.end(), [](const auto &v) { return v.kind == 'T'; })); });
    mutate("moved text", [&](auto &r) { for (auto &v : r) if (v.kind == 'T') v.values[0] += 10; });
    mutate("changed version header", [&](auto &r) { for (auto &v : r) if (v.kind == 'v') v.attributes += " changed=1"; });
    mutate("changed global", [&](auto &r) { for (auto &v : r) if (v.kind == 'G') v.attributes = "changed=1"; });
    mutate("added attribute key", [&](auto &r) { instance(r).attributes += " extra=1"; });
    for (const std::string missing : {"xschem.native.layer", "xschem.native.sizeY", "xschem.native.attributes", "xschem.native.rotate", "xschem.native.flip"}) {
        auto corrupt = db;
        auto &props = corrupt.lib().cells()[0].contents()[0].block().shapes()[0].properties();
        props.erase(std::remove_if(props.begin(), props.end(), [&](const auto &p) { return p.name == missing; }), props.end());
        const auto corruptPath = target.parent_path() / ("corrupt-" + missing + ".room");
        corrupt.saveToFile(corruptPath.string(), room::ViewType::Schematic, {false});
        Document damaged(corruptPath); std::vector<Record> received; e = nullptr;
        ok = rx_read(damaged.doc, collect, &received, &e);
        require(!ok && received.empty(), "Corrupt metadata accepted: " + missing);
        auto msg = rx_error_message(e);
        require(std::string(msg.data, msg.size).find("metadata") != std::string::npos, "Missing corrupt metadata error");
        rx_error_free(e);
    }
    std::cout << "PASS corrupt native metadata groups return errors without crashing\n";
    for (const auto invalid : {"magnification", "duplicate identity"}) {
        auto unsupported = db;
        auto &instances = unsupported.lib().cells()[0].contents()[0].block().instances();
        if (std::string(invalid) == "magnification") instances[0].transform().mag = 2;
        else instances.push_back(inst);
        const auto path = target.parent_path() / (std::string("invalid-") + invalid + ".room");
        unsupported.saveToFile(path.string(), room::ViewType::Schematic, {compact});
        Document damaged(path); std::vector<Record> received; e = nullptr;
        ok = rx_read(damaged.doc, collect, &received, &e);
        require(!ok && received.empty(), std::string("Unsupported open accepted: ") + invalid);
        require(rx_error_message(e).size > 0, "Missing unsupported open error"); rx_error_free(e);
    }
    std::cout << "PASS magnification and ambiguous identities refused before object delivery\n";

    // An unrelated writer publishes newer bytes after the session opens.
    Document stale(target); auto staleRecords = read(stale); stage(stale, staleRecords);
    { std::ofstream out(target, std::ios::binary | std::ios::app); out << 'x'; }
    const auto newer = bytes(target); e = nullptr; ok = rx_commit(stale.doc, &e);
    require(!ok, "Stale revision was accepted");
    auto message = rx_error_message(e); require(std::string(message.data, message.size).find("conflict") != std::string::npos, "Missing conflict diagnostic"); rx_error_free(e);
    require(bytes(target) == newer, "Stale save changed original bytes");
    std::cout << "PASS stale revision refused; original byte-identical\n";
    { std::ofstream out(target, std::ios::binary); out << savedBytes; }
    Document failure(target); auto failureRecords = read(failure);
    auto &failureAttrs = instance(failureRecords).attributes;
    const auto valueStart = failureAttrs.find("value=") + 6;
    failureAttrs.replace(valueStart, failureAttrs.find(' ', valueStart) - valueStart, "999");
    stage(failure, failureRecords);
    struct rlimit limit, originalLimit; require(!getrlimit(RLIMIT_FSIZE, &originalLimit), "getrlimit");
    limit = originalLimit; limit.rlim_cur = 1;
    auto previous = std::signal(SIGXFSZ, SIG_IGN);
    require(!setrlimit(RLIMIT_FSIZE, &limit), "inject write failure");
    e = nullptr; ok = rx_commit(failure.doc, &e);
    require(!setrlimit(RLIMIT_FSIZE, &originalLimit), "restore file limit"); std::signal(SIGXFSZ, previous);
    require(!ok, "Injected write failure was accepted"); rx_error_free(e);
    require(bytes(target) == savedBytes, "Write failure changed original bytes");
    std::cout << "PASS injected write failure; original byte-identical\n";
}

// Exercise decoded parameter values with and without exact-attribute metadata.
void escapedValues(const fs::path &target)
{
    for (bool metadata : {false, true}) {
        room::Database db;
        auto &view = db.lib().getOrCreateCell("flat").getOrCreateContent(room::ViewType::Schematic);
        room::Instance inst("devices/res.sym", {});
        inst.properties() = {{"name", "R1"}, {"value", "10"}};
        if (metadata) inst.properties().push_back({"xschem.native.attributes", "name=R1 value=10"});
        view.block().instances().push_back(inst);
        db.saveToFile(target.string(), room::ViewType::Schematic);
        for (const std::string value : {"{braces}", "a\"b", "a\\b", "ends\\", "a b", "a\nb"}) {
            db.saveToFile(target.string(), room::ViewType::Schematic);
            Document doc(target); auto records = read(doc);
            std::string quoted = "\"";
            for (char c : value) { if (c == '\\' || c == '"') quoted += '\\'; quoted += c; }
            quoted += '"';
            for (auto &r : records) if (r.kind == 'C') {
                auto body = room::xschem::decodeString(r.attributes);
                body.replace(body.find("value=10"), 8, "value=" + quoted);
                r.attributes = room::xschem::encodeString(body);
            }
            stage(doc, records); rx_error *e = nullptr; int ok = rx_commit(doc.doc, &e); checked(ok, e);
            auto saved = room::Database::loadFromFile(target.string());
            const auto &props = saved.lib().cells()[0].contents()[0].block().instances()[0].properties();
            require(props[1].value == value, "Escaped value did not round-trip exactly");
            Document reopened(target); auto again = read(reopened); stage(reopened, again);
            ok = rx_commit(reopened.doc, &e); checked(ok, e);
        }
        // Reject CR before publication, including an outer escaped CR.
        for (const std::string cr : {"\r", "\\\r"}) {
            Document doc(target); auto records = read(doc);
            const auto original = bytes(target);
            require(room::xschem::decodeString("a" + cr + "b") == "a\rb", "Decoder lost CR");
            rx_error *e = nullptr; checked(rx_begin(doc.doc, &e), e);
            bool refused = false;
            for (auto &r : records) if (r.kind == 'C') {
                r.attributes = "name=R1 value=\"a" + cr + "b\"";
                rx_record c{r.kind, r.layer, r.rotation, r.flip, r.values.data(), r.values.size(), str(r.text), str(r.attributes)};
                refused = !rx_append(doc.doc, &c, &e);
                auto message = rx_error_message(e);
                require(std::string(message.data, message.size).find("Carriage returns") != std::string::npos,
                        "Missing CR diagnostic");
                rx_error_free(e);
            }
            require(refused && bytes(target) == original, "CR was accepted or changed original");
        }
        std::cout << "PASS CR refused without changing original; exact decoding (metadata=" << metadata << ")\n";
        std::cout << "PASS exact braces, quotes, backslashes, spaces and newlines (metadata=" << metadata << ")\n";
    }
}

// A writer waiting for the sibling lock must recheck the revision after acquiring it.
void lockedConflict(const fs::path &target)
{
    room::Database db;
    auto &v = db.lib().getOrCreateCell("flat").getOrCreateContent(room::ViewType::Schematic);
    room::Instance inst("devices/res.sym", {}); inst.properties() = {{"name", "R1"}, {"value", "10"}};
    v.block().instances().push_back(inst); db.saveToFile(target.string(), room::ViewType::Schematic);
    int gate[2]; require(pipe(gate) == 0, "pipe");
    int fd = open((target.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    require(fd >= 0 && flock(fd, LOCK_EX) == 0, "lock");
    auto child = fork(); require(child >= 0, "fork");
    if (!child) {
        close(fd); close(gate[0]);
        Document doc(target); auto r = read(doc);
        for (auto &record : r) if (record.kind == 'C') record.attributes.replace(record.attributes.find("value=10"), 8, "value=222");
        stage(doc, r); require(write(gate[1], "r", 1) == 1, "signal");
        rx_error *e = nullptr; int ok = rx_commit(doc.doc, &e);
        auto message = rx_error_message(e);
        bool conflict = std::string(message.data ? message.data : "", message.size).find("conflict") != std::string::npos;
        rx_error_free(e); _exit(!ok && conflict ? 0 : 1);
    }
    close(gate[1]); char ready; require(::read(gate[0], &ready, 1) == 1, "ready"); close(gate[0]);
    v.block().instances()[0].properties()[1].value = "999";
    db.saveToFile(target.string(), room::ViewType::Schematic);
    const auto external = bytes(target);
    require(flock(fd, LOCK_UN) == 0, "unlock"); close(fd);
    int status; require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "Waiting writer failed to refuse conflict");
    require(bytes(target) == external, "Concurrent update overwritten");
    std::cout << "PASS locked concurrent writer refused; external update retained\n";
}
int main(int argc, char **argv)
{
    try {
        require(argc >= 3, "Usage: native_test output_dir input.sch ...");
        fs::create_directories(argv[1]);
        for (int i = 2; i < argc; ++i) oracle(argv[i], fs::path(argv[1]) / (std::to_string(i) + ".room"));
        escapedValues(fs::path(argv[1]) / "escaped.room");
        lockedConflict(fs::path(argv[1]) / "locked.room");
        saves(fs::path(argv[1]) / "save.room", false);
        saves(fs::path(argv[1]) / "save-compact.room", true);
        return 0;
    } catch (const kj::Exception &e) { std::cerr << e.getDescription().cStr() << '\n'; }
      catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
    return 1;
}
