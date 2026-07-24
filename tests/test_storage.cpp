#include <cerrno>
#include <cstring>

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
