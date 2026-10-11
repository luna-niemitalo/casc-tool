#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// WoW ROOT manifest (MFST, "TSFM" on disk) parsing, independent of CascLib:
// `snapshot` gets ROOT bytes from a live storage, but a build's manifest must
// be re-derivable from its stored ROOT bytes alone (DESIGN.md invariant 3),
// with no storage to ask.
namespace rootmanifest {

using CKey = std::array<uint8_t, 16>;

struct Row {
    uint32_t fdid;
    uint32_t localeFlags;
    uint32_t contentFlags;
    CKey ckey;
};

// IN, critical: ROOT bytes, MFST version 2 only (every build since 10.1.7;
// both existing exports and 12.1.0 are v2). Rows in ROOT block order.
// Anything else fails with expected/actual in *error.
std::vector<Row> parse(const std::vector<uint8_t>& root, std::string* error);

// One CKey per FDID, chosen the way CascLib's WoW root handler picks for a
// locale mask (CascRootFile_WoW.cpp, ParseWowRootFile_Level1/2): skip
// DONT_LOAD and LOW_VIOLENCE blocks, keep blocks with no locale or one that
// includes `localeMask`, non-audio blocks before audio ones, first hit per
// FDID wins. Mirrored so the store serves the same bytes `extract` would.
std::unordered_map<uint32_t, CKey> select(const std::vector<Row>& rows, uint32_t localeMask);

std::string hex(const CKey& key);

// manifest.tsv: every row, sorted by FDID (ROOT order kept within one FDID).
// Derived from ROOT, for grep/join without casc-tool.
std::string toTsv(const std::vector<Row>& rows);

}  // namespace rootmanifest
