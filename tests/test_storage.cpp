#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <CascLib.h>

#include "storage.hpp"

TEST_SUITE("storage::looksLikeFileDataId") {

TEST_CASE("all-digit strings are FileDataIDs") {
    CHECK(storage::looksLikeFileDataId("123"));
    CHECK(storage::looksLikeFileDataId("0"));
    CHECK(storage::looksLikeFileDataId("007"));  // leading zeros still all-digit
}

TEST_CASE("empty string is not a FileDataID") {
    CHECK_FALSE(storage::looksLikeFileDataId(""));
}

TEST_CASE("anything with a non-digit character is not a FileDataID") {
    CHECK_FALSE(storage::looksLikeFileDataId("abc"));
    CHECK_FALSE(storage::looksLikeFileDataId("12a"));
    CHECK_FALSE(storage::looksLikeFileDataId("character/bloodelf/female/bloodelffemale.m2"));
    CHECK_FALSE(storage::looksLikeFileDataId(" 123"));
    CHECK_FALSE(storage::looksLikeFileDataId("123 "));
    CHECK_FALSE(storage::looksLikeFileDataId("-123"));  // minus sign isn't a digit
}

}  // TEST_SUITE("storage::looksLikeFileDataId")

TEST_SUITE("storage::parseLocale") {

TEST_CASE("known locale names map to their CASC_LOCALE_* constants") {
    CHECK(storage::parseLocale("all") == CASC_LOCALE_ALL);
    CHECK(storage::parseLocale("none") == CASC_LOCALE_NONE);
    CHECK(storage::parseLocale("dede") == CASC_LOCALE_DEDE);
    CHECK(storage::parseLocale("enus") == CASC_LOCALE_ENUS);
    CHECK(storage::parseLocale("kokr") == CASC_LOCALE_KOKR);
}

TEST_CASE("locale name matching is case-insensitive") {
    CHECK(storage::parseLocale("DEDE") == CASC_LOCALE_DEDE);
    CHECK(storage::parseLocale("DeDe") == CASC_LOCALE_DEDE);
    CHECK(storage::parseLocale("ALL") == CASC_LOCALE_ALL);
}

TEST_CASE("unknown locale name throws, naming the bad value and listing valid ones") {
    try {
        storage::parseLocale("klingon");
        FAIL("expected parseLocale to throw for an unknown locale name");
    } catch (const std::exception& e) {
        std::string msg = e.what();
        CHECK(msg.find("klingon") != std::string::npos);
        CHECK(msg.find("all") != std::string::npos);  // the valid-names list should be present
    }
}

}  // TEST_SUITE("storage::parseLocale")

TEST_SUITE("storage::errorMessage") {

TEST_CASE("errno-range codes match strerror on this platform") {
    CHECK(storage::errorMessage(ENOENT) == std::strerror(ENOENT));
    CHECK(storage::errorMessage(EACCES) == std::strerror(EACCES));
}

TEST_CASE("ERROR_FILE_ENCRYPTED points the user at --keys") {
    std::string msg = storage::errorMessage(ERROR_FILE_ENCRYPTED);
    CHECK(msg.find("encrypt") != std::string::npos);
    CHECK(msg.find("--keys") != std::string::npos);
}

TEST_CASE("ERROR_CKEY_ALREADY_OPENED is a distinct, non-generic message") {
    std::string msg = storage::errorMessage(ERROR_CKEY_ALREADY_OPENED);
    CHECK(msg != std::strerror(0));  // sanity: not accidentally falling through to a generic path
    CHECK(msg.find("already") != std::string::npos);
}

TEST_CASE("a completely unrecognized code doesn't crash and returns *some* text") {
    std::string msg = storage::errorMessage(0xDEADBEEF);
    CHECK_FALSE(msg.empty());
}

}  // TEST_SUITE("storage::errorMessage")

TEST_SUITE("storage::sanitizeRelativePath") {
// Regression coverage for FAILURES.md's former item 12 (now CHANGELOG.md):
// extract-batch's output path used to be built straight from a
// listfile-derived name with no boundary check of its own, relying
// entirely on an internal CascLib invariant.

TEST_CASE("an ordinary relative path passes through unchanged") {
    CHECK(storage::sanitizeRelativePath("character/bloodelf/female/bloodelffemale.m2") ==
          "character/bloodelf/female/bloodelffemale.m2");
}

TEST_CASE("backslashes (the game's own separator) become forward slashes") {
    CHECK(storage::sanitizeRelativePath("character\\bloodelf\\female\\bloodelffemale.m2") ==
          "character/bloodelf/female/bloodelffemale.m2");
}

TEST_CASE("a leading '..' component is dropped, not honored") {
    CHECK(storage::sanitizeRelativePath("../../../etc/passwd") == "etc/passwd");
}

TEST_CASE("a '..' component in the middle is dropped, not just a leading one") {
    CHECK(storage::sanitizeRelativePath("character/../../../etc/passwd") == "character/etc/passwd");
}

TEST_CASE("an absolute path loses its leading slash, it never escapes out-dir") {
    CHECK(storage::sanitizeRelativePath("/etc/passwd") == "etc/passwd");
}

TEST_CASE("a path that's only '..' components sanitizes to empty") {
    CHECK(storage::sanitizeRelativePath("../../..") == "");
}

TEST_CASE("a bare '.' component is dropped like '..' is") {
    CHECK(storage::sanitizeRelativePath("./character/./a.m2") == "character/a.m2");
}

TEST_CASE("a run of slashes collapses instead of producing empty path components") {
    CHECK(storage::sanitizeRelativePath("character//female///a.m2") == "character/female/a.m2");
}

TEST_CASE("empty input sanitizes to empty output") {
    CHECK(storage::sanitizeRelativePath("") == "");
}

}  // TEST_SUITE("storage::sanitizeRelativePath")

TEST_SUITE("storage::activeProducts") {

// Header and row shape of a real two-product install (_retail_ + _forever_,
// 2026-11-09), trimmed to the columns that matter plus one that doesn't.
const char* kBuildInfo =
    "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|Version!STRING:0|Product!STRING:0\n"
    "eu|1|bc493ecb2898766c0f8947a99b4b3d65|12.1.5.70783|wow\n"
    "eu|0|00000000000000000000000000000000|12.1.5.70688|wowt\n"
    "eu|1|9006ba6948388dd0d4a0578e64b8a92e|1.60.1.70790|wowf\n";

struct TempInstall {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "casc-tool-test-install";
    explicit TempInstall(const std::string& buildInfo) {
        std::filesystem::create_directories(dir);
        std::ofstream(dir / ".build.info") << buildInfo;
    }
    ~TempInstall() { std::filesystem::remove_all(dir); }
};

TEST_CASE("every Active=1 row, in file order, inactive ones skipped") {
    TempInstall install(kBuildInfo);
    std::string error;
    auto products = storage::activeProducts(install.dir, &error);
    CHECK(error.empty());
    REQUIRE(products.size() == 2);
    CHECK(products[0].product == "wow");
    CHECK(products[0].version == "12.1.5.70783");
    CHECK(products[1].product == "wowf");
    CHECK(products[1].version == "1.60.1.70790");
}

TEST_CASE("a header without a needed column names the column and shows the header") {
    TempInstall install("Branch!STRING:0|Active!DEC:1|Version!STRING:0\neu|1|12.1.5.70783\n");
    std::string error;
    CHECK(storage::activeProducts(install.dir, &error).empty());
    CHECK(error.find("'Product'") != std::string::npos);
    CHECK(error.find("Branch!STRING:0") != std::string::npos);
}

TEST_CASE("a missing .build.info is an error naming the path") {
    std::string error;
    CHECK(storage::activeProducts("/nonexistent-casc-tool-install", &error).empty());
    CHECK(error.find("/nonexistent-casc-tool-install/.build.info") != std::string::npos);
}

}  // TEST_SUITE("storage::activeProducts")
