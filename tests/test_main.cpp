// This file only provides doctest's main(); all actual test cases live in
// the other test_*.cpp files. Keeps compile times for the common case
// (re-running one test file) lower, and matches doctest's own convention.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
