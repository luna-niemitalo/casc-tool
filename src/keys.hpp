#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// TACT key harvesting from game data, independent of any CASC storage so it's
// unit-testable on byte buffers. Keys reach a client three ways (TACTKeys
// README): TactKey/TactKeyLookup DB2s shipped in a patch, hotfix rows for
// those two tables in Cache/ADB/<locale>/DBCache.bin, and key payloads
// appended to BroadcastText hotfixes (ADB.md, "optionalData"). The two DBCache
// channels exist only in a local cache, which is why this is worth re-running
// after play sessions.
namespace keys {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kTactKeyTableHash = 0xDF2F53CF;        // also the BroadcastText optionalData tag
constexpr uint32_t kTactKeyLookupTableHash = 0xAFC190D1;

// IN, critical: a WDC5 DB2 of exactly the TactKey/TactKeyLookup shape
// (one section, non-inline IDs, one fixed-size field). Anything else is
// rejected with expected/actual in *error rather than guessed at.
// Returns record ID -> raw record bytes.
std::map<uint32_t, Bytes> parseKeyTable(const Bytes& db2, uint32_t wantTableHash, uint32_t wantRecordSize,
                                        std::string* error);

struct DbCacheKeys {
    std::map<uint32_t, Bytes> keyRows;      // TactKey hotfix rows: ID -> 16-byte key
    std::map<uint32_t, Bytes> nameRows;     // TactKeyLookup hotfix rows: ID -> 8-byte name
    std::vector<std::pair<Bytes, Bytes>> optionalPairs;  // BroadcastText payloads: (name, key)
    size_t entries = 0;
};

// IN, critical: DBCache.bin, format version 9 only (ADB.md, >= 10.0.5).
// Fails on any other version or on lost entry sync, with the offset in *error.
DbCacheKeys parseDbCache(const Bytes& file, std::string* error);

// Key names as TACTKeys/CascLib write them: the 8 name bytes read as a
// little-endian u64, printed as 16 uppercase hex digits.
std::string nameHex(const Bytes& name8);
std::string keyHex(const Bytes& key16);

// Names already present in a TACTKeys-format file (first 16 chars per line).
// Upstream WoW.txt carries some names in both byte orders, so both orders of
// every listed name count as known.
std::set<std::string> loadKnownNames(const std::string& path, std::string* error);

enum class Status { New, Known, NameOnly };

struct Harvested {
    std::string name, key;  // key empty for NameOnly
    Status status;
    std::string source;
};

// Joins the DB2 tables and DBCache by record ID (DBCache rows override the
// shipped DB2, same as the client applies hotfixes), adds BroadcastText
// pairs, and classifies each against `known`. One row per distinct name.
std::vector<Harvested> harvest(const std::map<uint32_t, Bytes>& db2Keys, const std::map<uint32_t, Bytes>& db2Names,
                               const DbCacheKeys& cache, const std::set<std::string>& known);

}  // namespace keys
