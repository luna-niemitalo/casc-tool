#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unordered_map>

#include <doctest/doctest.h>

#include "rootmanifest.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace {

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t raw[4];
    std::memcpy(raw, &v, 4);
    b.insert(b.end(), raw, raw + 4);
}

struct Block {
    uint32_t locale, flags1, flags2;
    uint8_t flags3;
    std::vector<std::pair<uint32_t, uint8_t>> files;  // fdid, ckey fill byte
};

// MFST v2 with NoNameHash blocks (what 12.x ships): delta-coded FDIDs.
std::vector<uint8_t> makeRoot(const std::vector<Block>& blocks) {
    std::vector<uint8_t> b = {'T', 'S', 'F', 'M'};
    putU32(b, 24);  // header size
    putU32(b, 2);   // version
    putU32(b, 0);
    putU32(b, 0);
    putU32(b, 0);
    for (const auto& blk : blocks) {
        putU32(b, uint32_t(blk.files.size()));
        putU32(b, blk.locale);
        putU32(b, blk.flags1);
        putU32(b, blk.flags2 | 0x10000000);  // NoNameHash
        b.push_back(blk.flags3);
        int64_t prev = -1;
        for (const auto& [fdid, _] : blk.files) {
            putU32(b, uint32_t(int64_t(fdid) - prev - 1));
            prev = fdid;
        }
        for (const auto& [_, fill] : blk.files) b.insert(b.end(), 16, fill);
    }
    return b;
}

rootmanifest::CKey filledKey(uint8_t v) {
    rootmanifest::CKey k;
    k.fill(v);
    return k;
}

class TempStore {
public:
    TempStore() : path_(fs::temp_directory_path() / ("casc-tool-test-store-" + std::to_string(counter_++))) {}
    ~TempStore() { fs::remove_all(path_); }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

// md5("abc"), RFC 1321 test vector.
const char* kAbcMd5 = "900150983cd24fb0d6963f7d28e17f72";

rootmanifest::CKey keyFromHex(const std::string& hex) {
    rootmanifest::CKey k;
    for (size_t i = 0; i < 16; i++) k[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return k;
}

}  // namespace

TEST_SUITE("rootmanifest") {

TEST_CASE("parse decodes delta-coded FDIDs across blocks") {
    auto root = makeRoot({{0x2, 0, 0, 0, {{10, 1}, {11, 2}, {20, 3}}}, {0x2, 0, 0, 0, {{5, 4}}}});
    std::string error;
    auto rows = rootmanifest::parse(root, &error);
    CHECK(error.empty());
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].fdid == 10);
    CHECK(rows[2].fdid == 20);
    CHECK(rows[2].ckey == filledKey(3));
    CHECK(rows[3].fdid == 5);
}

TEST_CASE("parse rejects non-v2 roots and truncated blocks with expected/actual") {
    std::string error;
    auto root = makeRoot({});
    root[8] = 1;
    rootmanifest::parse(root, &error);
    CHECK(error.find("version 1") != std::string::npos);

    error.clear();
    auto truncated = makeRoot({{0x2, 0, 0, 0, {{1, 1}, {2, 2}}}});
    truncated.resize(truncated.size() - 5);
    rootmanifest::parse(truncated, &error);
    CHECK(error.find("past file size") != std::string::npos);
}

TEST_CASE("select mirrors CascLib: skips low-violence/don't-load and other locales, non-audio first, first wins") {
    auto root = makeRoot({
        {0x2, 0x80, 0, 0, {{1, 0xAA}}},          // low violence: skipped
        {0x2, 0x100, 0, 0, {{2, 0xAA}}},         // don't load: skipped
        {0x10, 0, 0, 0, {{3, 0xAA}}},            // deDE only: skipped for enUS
        {0x2, 0, 0x80000000, 0, {{4, 0xBB}}},    // audio block, listed first...
        {0x2, 0, 0, 0, {{1, 0x11}, {4, 0x44}}},  // ...but non-audio wins for 4
        {0x0, 0, 0, 0, {{3, 0x33}}},             // no locale: allowed
        {0x2, 0, 0, 0, {{1, 0x99}}},             // later duplicate: ignored
    });
    std::string error;
    auto chosen = rootmanifest::select(rootmanifest::parse(root, &error), 0x2);
    CHECK(error.empty());
    CHECK(chosen.size() == 3);
    CHECK(chosen.at(1) == filledKey(0x11));
    CHECK(chosen.count(2) == 0);
    CHECK(chosen.at(3) == filledKey(0x33));
    CHECK(chosen.at(4) == filledKey(0x44));
}

TEST_CASE("compare classifies every FDID of either side, sorted by FDID") {
    std::unordered_map<uint32_t, rootmanifest::CKey> older = {{3, filledKey(1)}, {1, filledKey(2)}, {7, filledKey(3)}};
    std::unordered_map<uint32_t, rootmanifest::CKey> newer = {{1, filledKey(2)}, {3, filledKey(9)}, {5, filledKey(4)}};
    auto d = rootmanifest::compare(older, newer);
    REQUIRE(d.size() == 4);
    using C = rootmanifest::Change;
    CHECK((d[0].fdid == 1 && d[0].change == C::Unchanged));
    CHECK((d[1].fdid == 3 && d[1].change == C::Changed && d[1].a == filledKey(1) && d[1].b == filledKey(9)));
    CHECK((d[2].fdid == 5 && d[2].change == C::Added && d[2].b == filledKey(4)));
    CHECK((d[3].fdid == 7 && d[3].change == C::Removed && d[3].a == filledKey(3)));
}

}  // TEST_SUITE

TEST_SUITE("store") {

TEST_CASE("putBytes stores under objects/ab/cd/<ckey>, read-only, only when the hash matches") {
    TempStore tmp;
    store::Store st(tmp.path());
    auto key = keyFromHex(kAbcMd5);
    std::string error;
    REQUIRE(st.putBytes(key, reinterpret_cast<const uint8_t*>("abc"), 3, &error));
    CHECK(st.objectPath(key) == tmp.path() / "objects" / "90" / "01" / kAbcMd5);
    CHECK(st.hasObject(key));
    auto perms = fs::status(st.objectPath(key)).permissions();
    CHECK((perms & fs::perms::owner_write) == fs::perms::none);
}

TEST_CASE("putBytes refuses content that doesn't match the key, leaving nothing behind") {
    TempStore tmp;
    store::Store st(tmp.path());
    auto key = keyFromHex(kAbcMd5);
    std::string error;
    CHECK_FALSE(st.putBytes(key, reinterpret_cast<const uint8_t*>("abd"), 3, &error));
    CHECK(error.find(kAbcMd5) != std::string::npos);
    CHECK_FALSE(st.hasObject(key));
    CHECK(fs::is_empty(tmp.path() / "tmp"));
}

TEST_CASE("adoptFile clones a wanted file and leaves the source in place") {
    TempStore tmp;
    store::Store st(tmp.path());
    fs::create_directories(tmp.path() / "legacy");
    fs::path src = tmp.path() / "legacy" / "a.bin";
    std::ofstream(src) << "abc";
    std::string error;
    auto key = st.adoptFile(src, store::Store::Adopt::Clone, [](const auto&) { return true; }, &error);
    REQUIRE(key);
    CHECK(*key == keyFromHex(kAbcMd5));
    CHECK(fs::exists(src));
    CHECK(fs::file_size(st.objectPath(*key)) == 3);
}

TEST_CASE("adoptFile moves a wanted file, and never touches an unwanted one") {
    TempStore tmp;
    store::Store st(tmp.path());
    fs::create_directories(tmp.path() / "legacy");
    fs::path wantedSrc = tmp.path() / "legacy" / "a.bin";
    fs::path staleSrc = tmp.path() / "legacy" / "stale.bin";
    std::ofstream(wantedSrc) << "abc";
    std::ofstream(staleSrc) << "old content";
    auto onlyAbc = [](const rootmanifest::CKey& k) { return k == keyFromHex(kAbcMd5); };
    std::string error;
    CHECK(st.adoptFile(wantedSrc, store::Store::Adopt::Move, onlyAbc, &error));
    CHECK_FALSE(fs::exists(wantedSrc));
    CHECK(st.hasObject(keyFromHex(kAbcMd5)));
    CHECK_FALSE(st.adoptFile(staleSrc, store::Store::Adopt::Move, onlyAbc, &error));
    CHECK(fs::exists(staleSrc));
}

TEST_CASE("tags and builds are read back from the layout") {
    TempStore tmp;
    store::Store st(tmp.path());
    fs::create_directories(st.buildDir("12.1.0.69933"));
    fs::create_directories(st.buildDir("12.1.0.68914"));
    st.setTag("live", "12.1.0.69933");
    CHECK(st.builds() == std::vector<std::string>{"12.1.0.68914", "12.1.0.69933"});
    CHECK(st.tags().at("live") == "12.1.0.69933");
}

TEST_CASE("labelLess orders digit runs numerically, so builds list oldest first") {
    CHECK(store::labelLess("12.1.5.70783", "12.1.10.1"));
    CHECK(store::labelLess("1.60.1.70790", "12.0.7.68887"));
    CHECK_FALSE(store::labelLess("12.1.5.70783", "12.1.5.70783"));
    CHECK(store::labelLess("12.1.5", "12.1.5.1"));
    CHECK(store::labelLess("12.1.5.70783", "legacy-wow_export"));
    TempStore tmp;
    store::Store st(tmp.path());
    for (const char* l : {"12.1.10.1", "12.1.5.70783", "1.60.1.70790"}) fs::create_directories(st.buildDir(l));
    CHECK(st.builds() == std::vector<std::string>{"1.60.1.70790", "12.1.5.70783", "12.1.10.1"});
}

TEST_CASE("resolve takes a label or a tag, and a label is never shadowed by a tag") {
    TempStore tmp;
    store::Store st(tmp.path());
    fs::create_directories(st.buildDir("1.60.1.70790"));
    fs::create_directories(st.buildDir("forever"));
    st.setTag("live", "1.60.1.70790");
    st.setTag("forever", "1.60.1.70790");
    CHECK(st.resolve("1.60.1.70790") == std::optional<std::string>("1.60.1.70790"));
    CHECK(st.resolve("live") == std::optional<std::string>("1.60.1.70790"));
    CHECK(st.resolve("forever") == std::optional<std::string>("forever"));
    CHECK_FALSE(st.resolve("ptr"));
}

TEST_CASE("meta reads the flat meta.json snapshot and import write; absent keys are absent") {
    TempStore tmp;
    store::Store st(tmp.path());
    fs::create_directories(st.buildDir("1.60.1.70790"));
    std::ofstream(st.buildDir("1.60.1.70790") / "meta.json")
        << "{\n  \"label\": \"1.60.1.70790\",\n  \"product\": \"wowf\",\n  \"fdids\": 1244644,\n"
           "  \"snapshot_at\": \"2026-11-09T16:00:00Z\"\n}\n";
    auto m = st.meta("1.60.1.70790");
    CHECK(m.at("product") == "wowf");
    CHECK(m.at("fdids") == "1244644");
    CHECK(m.at("snapshot_at") == "2026-11-09T16:00:00Z");
    CHECK(m.count("kind") == 0);
    CHECK(st.meta("not-stored").empty());
}

TEST_CASE("readRoot parses a stored ROOT and names a path build's lack of one") {
    TempStore tmp;
    store::Store st(tmp.path());
    std::string error;
    auto root = makeRoot({{0x2, 0, 0, 0, {{10, 1}}}});
    REQUIRE(st.recordRoot("12.1.5.70783", root, rootmanifest::parse(root, &error), &error));
    auto rows = st.readRoot("12.1.5.70783", &error);
    CHECK(error.empty());
    CHECK(rows.size() == 1);
    fs::create_directories(st.buildDir("legacy"));
    CHECK(st.readRoot("legacy", &error).empty());
    CHECK(error.find("no ROOT") != std::string::npos);
}

}  // TEST_SUITE
