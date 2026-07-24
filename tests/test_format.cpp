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

// Regression tests for a bug found by hand: jsonEscape only special-cases
// '"', '\\', '\n', '\r', '\t'. Per RFC 8259, every C0 control character
// (U+0000-U+001F) must be escaped in a JSON string -- the rest currently
// pass through raw, which would emit invalid JSON if a listfile name ever
// contained one of these bytes (not seen in today's real community
// listfile, but nothing here guards against it).

TEST_CASE("C0 control characters without a short escape become \\u00XX (RFC 8259)") {
    CHECK(format::jsonEscape("\x01") == "\\u0001");
    CHECK(format::jsonEscape("\x08") == "\\u0008");  // backspace
    CHECK(format::jsonEscape("\x0c") == "\\u000c");  // form feed
    CHECK(format::jsonEscape("\x1f") == "\\u001f");
}

TEST_CASE("every C0 control character except \\n \\r \\t is escaped somehow, never emitted raw") {
    for (int c = 0; c <= 0x1F; c++) {
        if (c == '\n' || c == '\r' || c == '\t') continue;
        std::string input(1, static_cast<char>(c));
        CAPTURE(c);
        CHECK(format::jsonEscape(input) != input);
    }
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
