#pragma once



#include "block.h"

#include "cell.h"

#include "cell_content.h"

#include "source_info.h"



#include <optional>
#include <ostream>
#include <string>
#include <vector>



namespace room::xschem {



// Typed editor records; coordinates are Xschem units, strings contain no outer braces.
struct Record {
    char kind = 0;
    int layer = 0, rotation = 0, flip = 0;
    std::vector<double> values;
    std::string text, attributes;
};

// Encode/decode the outer Xschem string layer (save/load_ascii_string rules).
std::string encodeString(const std::string &value);
std::string decodeString(const std::string &value);

// Collect the same mapped objects used by the text exporter, without text serialization.
std::vector<Record> collectRecords(const Cell &cell, const CellContent &content, bool native = false);
Record parseRecord(const std::string &record);
void formatRecord(std::ostream &out, const Record &record);

std::vector<std::string> readRecords(const std::string &path, std::vector<std::string> &errors);
std::vector<std::string> readRecordsFromText(const std::string &text, std::vector<std::string> &errors);

void importRecords(const std::vector<std::string> &records, Cell &cell, CellContent &content,

                   std::vector<std::string> &warnings);

void exportRecords(std::ostream &out, const Cell &cell, const CellContent &content);

// Normalize instances written by Qucs ROOM save so Xschem can resolve symbols.
void annotateInstanceForStorage(Instance &inst);
void annotateBlockForStorage(Block &block);

// Build Xschem graph B-record from Qucs diagram properties when available.
std::optional<std::string> graphRecordForContent(const Block &block, const CellContent &content);

// Convert Xschem graph B-record to Qucs <Diagrams> lines (ngspice probe names).
std::vector<std::string> graphRecordToDiagramLines(const std::string &graphRecord, const Block &block);

enum class GraphSyncDirection {
    FromQucsDiagram,  // Qucs save/import: diagrams geometry is authoritative
    FromXschemGraph,  // Xschem save/import: graph geometry is authoritative
    DeriveMissing,    // Export/open: only create the missing representation
};

// Keep section.graph (Xschem) and section.Diagrams (Qucs) in sync for dual-tool cells.
void syncDualToolGraphProperties(Block &block, CellContent &content,
                                 GraphSyncDirection direction = GraphSyncDirection::DeriveMissing);

bool isValidGraphRecord(const std::string &record);
void replaceSectionLines(std::vector<Property> &props, const std::string &name, const std::vector<std::string> &lines);
void copyAllSectionLinesIfMissing(std::vector<Property> &dest, const std::vector<Property> &src,
                                  const std::string &name);
void removeInvalidGraphProperties(std::vector<Property> &props);

} // namespace room::xschem

