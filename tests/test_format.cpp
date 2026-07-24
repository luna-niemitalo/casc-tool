#include <doctest/doctest.h>

#include "format.hpp"

TEST_SUITE("format::jsonEscape") {

TEST_CASE("plain ASCII passes through unchanged") {
    CHECK(format::jsonEscape("character/bloodelf/female/bloodelffemale.m2") ==
          "character/bloodelf/female/bloodelffemale.m2");
}

TEST_CASE("backslash is escaped (WoW paths from CascLib use backslashes)") {
    CHECK(format::jsonEscape("character\\bloodelf\\female\\bloodelffemale.m2") ==
          "character\\\\bloodelf\\\\female\\\\bloodelffemale.m2");
}

TEST_CASE("double quote is escaped") {
    CHECK(format::jsonEscape("a\"b") == "a\\\"b");
}

TEST_CASE("newline, carriage return, and tab are escaped") {
    CHECK(format::jsonEscape("a\nb\rc\td") == "a\\nb\\rc\\td");
}

TEST_CASE("empty string stays empty") {
    CHECK(format::jsonEscape("") == "");
}

}  // TEST_SUITE("format::jsonEscape")

TEST_SUITE("format::csvEscape") {

TEST_CASE("plain string is not quoted at all") {
    CHECK(format::csvEscape("plain") == "plain");
}

TEST_CASE("a comma forces quoting") {
    CHECK(format::csvEscape("a,b") == "\"a,b\"");
}

TEST_CASE("a double quote forces quoting and gets doubled") {
    CHECK(format::csvEscape("a\"b") == "\"a\"\"b\"");
}

TEST_CASE("a newline forces quoting") {
    CHECK(format::csvEscape("a\nb") == "\"a\nb\"");
}

TEST_CASE("a backslash alone does not force quoting (CSV has no escape character)") {
    CHECK(format::csvEscape("character\\bloodelf") == "character\\bloodelf");
}

TEST_CASE("empty string stays empty and unquoted") {
    CHECK(format::csvEscape("") == "");
}

}  // TEST_SUITE("format::csvEscape")
