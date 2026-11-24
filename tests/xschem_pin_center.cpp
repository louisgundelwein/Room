// Exported Xschem pin boxes keep their size and only move to put the pin center on the
// editor-unit grid, so wires that end on grid points stay connected.

#include "database.h"
#include "serialization.h"
#include "xschem_exporter.h"
#include "xschem_importer.h"

#include <capnp/message.h>

#include <iostream>
#include <string>

namespace {

bool exportsPin(const std::string &pin, const std::string &expected)
{
    const std::string symbol = "v {xschem version=3.4.5 file_version=1.2}\nK {type=subcircuit}\n"
                               "B 5 " + pin + " {name=P dir=inout pinnumber=1}\n";
    room::XschemImporter importer;
    const room::Database db = importer.importText(symbol, ".sym", "pin");
    if (!importer.errors().empty()) {
        std::cerr << "import failed: " << importer.errors().front() << '\n';
        return false;
    }
    room::XschemExporter exporter;
    const std::string text = exporter.exportCellToString(db, "pin");
    if (text.find("B 5 " + expected + " {") == std::string::npos) {
        std::cerr << "pin " << pin << " should export as " << expected << ", got:\n" << text << '\n';
        return false;
    }
    return true;
}

bool preservesPinOrder()
{
    const std::string pins =
        "B 5 17.5 -32.5 22.5 -27.5 {name=D dir=inout}\n"
        "B 5 -22.5 -2.5 -17.5 2.5 {name=G dir=in}\n"
        "B 5 17.5 27.5 22.5 32.5 {name=S dir=inout}\n"
        "B 5 19.5 -0.5 20.5 0.5 {name=B dir=in}\n";
    room::XschemImporter importer;
    const room::Database original = importer.importText("K {type=nmos}\n" + pins, ".sym", "nmos");
    if (!importer.errors().empty()) {
        std::cerr << "import failed: " << importer.errors().front() << '\n';
        return false;
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<room::schema::Database>();
    room::writeDatabase(root, original);
    const room::Database restored = room::readDatabase(root.asReader());
    room::XschemExporter exporter;
    const std::string exported = exporter.exportCellToString(restored, "nmos");
    if (exported.find(pins) == std::string::npos) {
        std::cerr << "pin order changed during ROOM storage, got:\n" << exported;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    // res.sym: half-unit edges, center already on the grid.
    if (!exportsPin("-2.5 -32.5 2.5 -27.5", "-2.5 -32.5 2.5 -27.5")) {
        return 1;
    }
    // Off-grid center snaps to the grid.
    if (!exportsPin("-2.3 -2.3 2.7 2.7", "-2.5 -2.5 2.5 2.5")) {
        return 2;
    }
    if (!preservesPinOrder()) {
        return 3;
    }

    std::cout << "xschem pin center and order OK\n";
    return 0;
}
