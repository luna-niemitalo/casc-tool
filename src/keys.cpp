#include "keys.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace keys {

namespace {

uint32_t u32(const Bytes& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

std::string hexBytes(const uint8_t* p, size_t n, bool reversed) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[reversed ? n - 1 - i : i];
        s += digits[c >> 4];
        s += digits[c & 0xF];
    }
    return s;
}

bool allZero(const Bytes& b) {
    return std::all_of(b.begin(), b.end(), [](uint8_t c) { return c == 0; });
}

// WDC5 header offsets (DB2.md): 4 magic, 4 version, 128 schema string, then u32 fields.
constexpr size_t kRecordCount = 136, kRecordSize = 144, kStringTableSize = 148, kTableHash = 152;
constexpr size_t kSectionCount = 200, kHeaderSize = 204, kSectionFileOffset = 8;  // within section header

// DBCache.bin v9 (ADB.md): 44-byte file header; 32-byte entry header
// [magic, region, push index, unique id, table hash, record id, data size, status+pad].
constexpr size_t kCacheHeader = 44, kEntryHeader = 32;
constexpr size_t kEntryTable = 16, kEntryRecord = 20, kEntrySize = 24, kEntryStatus = 28;
constexpr uint8_t kStatusValid = 1;
constexpr size_t kOptionalPairSize = 4 + 8 + 16;  // tag, TACTID, key

}  // namespace

std::map<uint32_t, Bytes> parseKeyTable(const Bytes& db2, uint32_t wantTableHash, uint32_t wantRecordSize,
                                        std::string* error) {
    if (db2.size() < kHeaderSize + 40 || std::memcmp(db2.data(), "WDC5", 4) != 0) {
        *error = "expected a WDC5 DB2 of at least " + std::to_string(kHeaderSize + 40) + " bytes, got " +
                 std::to_string(db2.size()) + " bytes starting '" + std::string(db2.begin(), db2.begin() + std::min<size_t>(4, db2.size())) + "'";
        return {};
    }
    uint32_t hash = u32(db2, kTableHash), recordSize = u32(db2, kRecordSize), sections = u32(db2, kSectionCount);
    if (hash != wantTableHash || recordSize != wantRecordSize || sections != 1) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "expected table hash %08X, record size %u, 1 section; got %08X, %u, %u",
                      wantTableHash, wantRecordSize, hash, recordSize, sections);
        *error = buf;
        return {};
    }
    uint32_t records = u32(db2, kRecordCount);
    size_t dataAt = u32(db2, kHeaderSize + kSectionFileOffset);
    size_t idsAt = dataAt + size_t(records) * recordSize + u32(db2, kStringTableSize);
    if (idsAt + 4 * size_t(records) > db2.size()) {
        *error = "ID list ends at " + std::to_string(idsAt + 4 * size_t(records)) + ", past file size " +
                 std::to_string(db2.size());
        return {};
    }
    std::map<uint32_t, Bytes> rows;
    for (uint32_t i = 0; i < records; i++) {
        auto rec = db2.begin() + dataAt + size_t(i) * recordSize;
        rows[u32(db2, idsAt + 4 * i)] = Bytes(rec, rec + recordSize);
    }
    return rows;
}

DbCacheKeys parseDbCache(const Bytes& file, std::string* error) {
    DbCacheKeys out;
    if (file.size() < kCacheHeader || std::memcmp(file.data(), "XFTH", 4) != 0 || u32(file, 4) != 9) {
        *error = "expected DBCache.bin 'XFTH' version 9, got " +
                 (file.size() >= 8 ? "version " + std::to_string(u32(file, 4)) : std::to_string(file.size()) + " bytes");
        return {};
    }
    size_t at = kCacheHeader;
    while (at < file.size()) {
        if (at + kEntryHeader > file.size() || std::memcmp(file.data() + at, "XFTH", 4) != 0) {
            *error = "lost entry sync at offset " + std::to_string(at) + " of " + std::to_string(file.size()) +
                     " after " + std::to_string(out.entries) + " entries";
            return {};
        }
        uint32_t table = u32(file, at + kEntryTable), record = u32(file, at + kEntryRecord);
        size_t size = u32(file, at + kEntrySize);
        size_t dataAt = at + kEntryHeader;
        if (dataAt + size > file.size()) {
            *error = "entry at offset " + std::to_string(at) + " claims " + std::to_string(size) +
                     " bytes, past file size " + std::to_string(file.size());
            return {};
        }
        auto data = file.begin() + dataAt;
        bool valid = file[at + kEntryStatus] == kStatusValid;
        if (valid && table == kTactKeyTableHash && size >= 16) out.keyRows[record] = Bytes(data, data + 16);
        if (valid && table == kTactKeyLookupTableHash && size >= 8) out.nameRows[record] = Bytes(data, data + 8);
        // optionalData has no size field (the client knows its layout), so
        // scan the payload for the tag; a false hit costs one bogus pair that
        // fails to decrypt anything, a missed real one costs a key.
        for (size_t o = 0; valid && size >= kOptionalPairSize && o + kOptionalPairSize <= size; o++) {
            if (u32(file, dataAt + o) != kTactKeyTableHash) continue;
            out.optionalPairs.emplace_back(Bytes(data + o + 4, data + o + 12), Bytes(data + o + 12, data + o + 28));
        }
        at = dataAt + size;
        out.entries++;
    }
    return out;
}

std::string nameHex(const Bytes& name8) { return hexBytes(name8.data(), 8, true); }
std::string keyHex(const Bytes& key16) { return hexBytes(key16.data(), 16, false); }

std::set<std::string> loadKnownNames(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        *error = "couldn't open keys file '" + path + "'";
        return {};
    }
    std::set<std::string> known;
    for (std::string line; std::getline(in, line);) {
        if (line.size() < 16) continue;
        std::string name = line.substr(0, 16);
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        std::string swapped;
        for (int i = 14; i >= 0; i -= 2) swapped += name.substr(i, 2);
        known.insert(name);
        known.insert(swapped);
    }
    return known;
}

std::vector<Harvested> harvest(const std::map<uint32_t, Bytes>& db2Keys, const std::map<uint32_t, Bytes>& db2Names,
                               const DbCacheKeys& cache, const std::set<std::string>& known) {
    std::map<std::string, Harvested> byName;
    auto add = [&](const Bytes& name, const Bytes* key, const std::string& source) {
        std::string n = nameHex(name);
        bool hasKey = key != nullptr && !allZero(*key);  // zero rows = encrypted DB2 section, no key yet
        auto& h = byName[n];
        if (h.name.empty()) h = {n, "", Status::NameOnly, source};
        else if (h.source.find(source) == std::string::npos) h.source += "," + source;
        if (hasKey && h.key.empty()) h.key = keyHex(*key);
    };

    std::map<uint32_t, Bytes> names = db2Names, keyRows = db2Keys;
    std::map<uint32_t, std::string> nameSource, keySource;
    for (auto& [id, _] : db2Names) nameSource[id] = "db2";
    for (auto& [id, _] : db2Keys) keySource[id] = "db2";
    for (auto& [id, n] : cache.nameRows) { names[id] = n; nameSource[id] = "hotfix"; }
    for (auto& [id, k] : cache.keyRows) { keyRows[id] = k; keySource[id] = "hotfix"; }

    for (auto& [id, name] : names) {
        auto k = keyRows.find(id);
        add(name, k == keyRows.end() ? nullptr : &k->second,
            nameSource[id] + (k == keyRows.end() ? "" : "+" + keySource[id]) + " id " + std::to_string(id));
    }
    for (auto& [name, key] : cache.optionalPairs) add(name, &key, "broadcasttext");

    std::vector<Harvested> out;
    for (auto& [n, h] : byName) {
        if (!h.key.empty()) h.status = known.count(n) ? Status::Known : Status::New;
        out.push_back(h);
    }
    return out;
}

}  // namespace keys
