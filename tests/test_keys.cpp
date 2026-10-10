#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include "keys.hpp"

using keys::Bytes;

namespace {

void putU32(Bytes& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

// Minimal WDC5 of the TactKey/TactKeyLookup shape: header, one section
// header, records at 0x110, 2-byte string table, then the ID list.
Bytes makeKeyTable(uint32_t tableHash, uint32_t recordSize, const std::vector<std::pair<uint32_t, Bytes>>& rows) {
    const size_t dataAt = 0x110;
    Bytes b(dataAt + rows.size() * recordSize + 2 + rows.size() * 4, 0);
    std::memcpy(b.data(), "WDC5", 4);
    putU32(b, 136, static_cast<uint32_t>(rows.size()));
    putU32(b, 144, recordSize);
    putU32(b, 148, 2);
    putU32(b, 152, tableHash);
    putU32(b, 200, 1);
    putU32(b, 204 + 8, dataAt);
    size_t idsAt = dataAt + rows.size() * recordSize + 2;
    for (size_t i = 0; i < rows.size(); i++) {
        std::memcpy(b.data() + dataAt + i * recordSize, rows[i].second.data(), recordSize);
        putU32(b, idsAt + 4 * i, rows[i].first);
    }
    return b;
}

Bytes makeDbCache(uint32_t version) {
    Bytes b(44, 0);
    std::memcpy(b.data(), "XFTH", 4);
    putU32(b, 4, version);
    return b;
}

void addEntry(Bytes& cache, uint32_t table, uint32_t record, const Bytes& data, uint8_t status = 1) {
    size_t at = cache.size();
    cache.resize(at + 32 + data.size(), 0);
    std::memcpy(cache.data() + at, "XFTH", 4);
    putU32(cache, at + 16, table);
    putU32(cache, at + 20, record);
    putU32(cache, at + 24, static_cast<uint32_t>(data.size()));
    cache[at + 28] = status;
    std::memcpy(cache.data() + at + 32, data.data(), data.size());
}

Bytes filled(size_t n, uint8_t v) { return Bytes(n, v); }
const Bytes kName = {0x9F, 0xD8, 0xFA, 0x48, 0x2B, 0xC7, 0x53, 0x3B};  // = 3B53C72B48FAD89F

}  // namespace

TEST_SUITE("keys") {

TEST_CASE("nameHex reads the 8 name bytes as a little-endian u64, like CascLib") {
    CHECK(keys::nameHex(kName) == "3B53C72B48FAD89F");
}

TEST_CASE("parseKeyTable maps record IDs to records") {
    auto db2 = makeKeyTable(keys::kTactKeyTableHash, 16, {{15, filled(16, 0xAA)}, {8289, filled(16, 0xBB)}});
    std::string error;
    auto rows = keys::parseKeyTable(db2, keys::kTactKeyTableHash, 16, &error);
    CHECK(error.empty());
    REQUIRE(rows.size() == 2);
    CHECK(rows.at(8289) == filled(16, 0xBB));
}

TEST_CASE("parseKeyTable rejects the wrong table, naming expected and actual") {
    auto db2 = makeKeyTable(keys::kTactKeyLookupTableHash, 8, {{1, filled(8, 1)}});
    std::string error;
    auto rows = keys::parseKeyTable(db2, keys::kTactKeyTableHash, 16, &error);
    CHECK(rows.empty());
    CHECK(error.find("DF2F53CF") != std::string::npos);
    CHECK(error.find("AFC190D1") != std::string::npos);
}

TEST_CASE("parseDbCache rejects versions other than 9") {
    std::string error;
    keys::parseDbCache(makeDbCache(8), &error);
    CHECK(error.find("version 8") != std::string::npos);
}

TEST_CASE("parseDbCache collects key rows, name rows and BroadcastText payloads, skipping non-valid entries") {
    auto cache = makeDbCache(9);
    addEntry(cache, keys::kTactKeyTableHash, 7, filled(16, 0x11));
    addEntry(cache, keys::kTactKeyLookupTableHash, 7, kName);
    addEntry(cache, keys::kTactKeyTableHash, 8, filled(16, 0x22), /*status Delete*/ 2);
    Bytes broadcast = filled(5, 0x41);  // record text before the optionalData
    Bytes tag(4);
    uint32_t t = keys::kTactKeyTableHash;
    std::memcpy(tag.data(), &t, 4);
    broadcast.insert(broadcast.end(), tag.begin(), tag.end());
    broadcast.insert(broadcast.end(), kName.begin(), kName.end());
    Bytes key = filled(16, 0x33);
    broadcast.insert(broadcast.end(), key.begin(), key.end());
    addEntry(cache, 0x12345678, 99, broadcast);

    std::string error;
    auto parsed = keys::parseDbCache(cache, &error);
    CHECK(error.empty());
    CHECK(parsed.entries == 4);
    CHECK(parsed.keyRows.size() == 1);
    CHECK(parsed.nameRows.at(7) == kName);
    REQUIRE(parsed.optionalPairs.size() == 1);
    CHECK(parsed.optionalPairs[0].second == key);
}

TEST_CASE("parseDbCache reports lost sync with its offset") {
    auto cache = makeDbCache(9);
    cache.resize(cache.size() + 40, 0);
    std::string error;
    keys::parseDbCache(cache, &error);
    CHECK(error.find("offset 44") != std::string::npos);
}

TEST_CASE("harvest classifies new, known and name-only; hotfix rows override DB2") {
    Bytes otherName = {1, 2, 3, 4, 5, 6, 7, 8};    // nameHex 0807060504030201
    Bytes thirdName = {9, 9, 9, 9, 9, 9, 9, 0x10};  // nameHex 1009090909090909
    std::map<uint32_t, Bytes> db2Names = {{1, kName}, {2, otherName}, {3, thirdName}};
    std::map<uint32_t, Bytes> db2Keys = {{1, filled(16, 0)}, {2, filled(16, 0x55)}};  // id 1: encrypted section
    keys::DbCacheKeys cache;
    cache.keyRows[2] = filled(16, 0x66);

    db2Names[4] = filled(8, 0x77);
    db2Keys[4] = filled(16, 0x88);
    std::set<std::string> known = {"0807060504030201"};
    auto rows = keys::harvest(db2Keys, db2Names, cache, known);
    std::map<std::string, keys::Harvested> byName;
    for (auto& r : rows) byName[r.name] = r;

    REQUIRE(rows.size() == 4);
    CHECK(byName["3B53C72B48FAD89F"].status == keys::Status::NameOnly);  // zero key row
    CHECK(byName["0807060504030201"].status == keys::Status::Known);
    CHECK(byName["0807060504030201"].key == "66666666666666666666666666666666");
    CHECK(byName["1009090909090909"].status == keys::Status::NameOnly);  // no key row at all
    CHECK(byName["7777777777777777"].status == keys::Status::New);
}

TEST_CASE("loadKnownNames accepts both byte orders of every listed name") {
    auto path = std::filesystem::temp_directory_path() / "casc-tool-test-keys.txt";
    {
        std::ofstream out(path);
        out << "3b53c72b48fad89f 00112233445566778899AABBCCDDEEFF\n";
    }
    std::string error;
    auto known = keys::loadKnownNames(path.string(), &error);
    std::filesystem::remove(path);
    CHECK(error.empty());
    CHECK(known.count("3B53C72B48FAD89F") == 1);
    CHECK(known.count("9FD8FA482BC7533B") == 1);
}

}  // TEST_SUITE
