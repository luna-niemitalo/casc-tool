#include <doctest/doctest.h>

#include "cli.hpp"

namespace {

std::vector<cli::OptionSpec> testSpecs() {
    return {
        {"--storage", true, "<path>", "storage path"},
        {"--verbose", false, "", "be verbose"},
    };
}

}  // namespace

TEST_SUITE("cli::Args") {

TEST_CASE("no arguments produces no flags, no options, no positionals") {
    cli::Args args({}, testSpecs());
    CHECK(args.positionals().empty());
    CHECK_FALSE(args.flag("--verbose"));
    CHECK_FALSE(args.option("--storage").has_value());
}

TEST_CASE("bare tokens are collected as positionals, in order") {
    cli::Args args({"first", "second", "third"}, testSpecs());
    REQUIRE(args.positionals().size() == 3);
    CHECK(args.positionals()[0] == "first");
    CHECK(args.positionals()[1] == "second");
    CHECK(args.positionals()[2] == "third");
}

TEST_CASE("boolean flag is false when absent, true when present") {
    cli::Args absent({}, testSpecs());
    CHECK_FALSE(absent.flag("--verbose"));

    cli::Args present({"--verbose"}, testSpecs());
    CHECK(present.flag("--verbose"));
}

TEST_CASE("value option accepts space-separated syntax") {
    cli::Args args({"--storage", "/some/path"}, testSpecs());
    REQUIRE(args.option("--storage").has_value());
    CHECK(*args.option("--storage") == "/some/path");
}

TEST_CASE("value option accepts --name=value inline syntax") {
    cli::Args args({"--storage=/some/path"}, testSpecs());
    REQUIRE(args.option("--storage").has_value());
    CHECK(*args.option("--storage") == "/some/path");
}

TEST_CASE("space-separated and inline forms of the same option produce the same value") {
    cli::Args spaced({"--storage", "x"}, testSpecs());
    cli::Args inline_({"--storage=x"}, testSpecs());
    CHECK(*spaced.option("--storage") == *inline_.option("--storage"));
}

TEST_CASE("optionOr returns the fallback when the option wasn't given") {
    cli::Args args({}, testSpecs());
    CHECK(args.optionOr("--storage", "default-path") == "default-path");
}

TEST_CASE("optionOr returns the actual value when the option was given") {
    cli::Args args({"--storage", "somewhere"}, testSpecs());
    CHECK(args.optionOr("--storage", "default-path") == "somewhere");
}

TEST_CASE("unknown option is rejected with a message naming the option") {
    CHECK_THROWS_WITH_AS(cli::Args({"--bogus"}, testSpecs()), doctest::Contains("--bogus"), cli::ArgError);
}

TEST_CASE("value option missing its value at end of input is rejected") {
    CHECK_THROWS_AS(cli::Args({"--storage"}, testSpecs()), cli::ArgError);
}

TEST_CASE("boolean flag given an inline value is rejected") {
    CHECK_THROWS_AS(cli::Args({"--verbose=true"}, testSpecs()), cli::ArgError);
}

TEST_CASE("flags and positionals can be interleaved in any order") {
    cli::Args args({"1234", "--storage", "foo", "out.m2", "--verbose"}, testSpecs());
    REQUIRE(args.positionals().size() == 2);
    CHECK(args.positionals()[0] == "1234");
    CHECK(args.positionals()[1] == "out.m2");
    CHECK(*args.option("--storage") == "foo");
    CHECK(args.flag("--verbose"));
}

TEST_CASE("requirePositionals rejects too few") {
    cli::Args args({"only-one"}, testSpecs());
    CHECK_THROWS_AS(args.requirePositionals(2, 2, "usage"), cli::ArgError);
}

TEST_CASE("requirePositionals rejects too many") {
    cli::Args args({"one", "two", "three"}, testSpecs());
    CHECK_THROWS_AS(args.requirePositionals(1, 2, "usage"), cli::ArgError);
}

TEST_CASE("requirePositionals accepts a count within [min, max]") {
    cli::Args args({"one", "two"}, testSpecs());
    CHECK_NOTHROW(args.requirePositionals(1, 2, "usage"));
}

TEST_CASE("requirePositionals with negative max means unbounded") {
    cli::Args args({"one", "two", "three", "four", "five"}, testSpecs());
    CHECK_NOTHROW(args.requirePositionals(1, -1, "usage"));
}

}  // TEST_SUITE("cli::Args")
