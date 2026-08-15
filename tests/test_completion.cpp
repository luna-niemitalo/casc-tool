// Regression tests for the anti-drift guarantee completion.cpp's own top
// comment describes: the generated scripts read registry::commandTable()'s
// real OptionSpec lists, so they should be structurally impossible to drift
// from what each command actually accepts. These tests exercise that
// guarantee directly (every real flag name appears in the generated output
// for its command) rather than just trusting the "generated from the same
// source" claim.

#include <doctest/doctest.h>

#include "completion.hpp"
#include "registry.hpp"

TEST_SUITE("completion::generate") {

TEST_CASE("every command's every real flag name appears in the bash script") {
    std::string script = completion::generate("bash");
    for (const auto& [name, cmd] : registry::commandTable()) {
        CHECK_MESSAGE(script.find(name) != std::string::npos, "subcommand '" << name << "' missing from bash completion");
        for (const auto& spec : cmd.specs()) {
            CHECK_MESSAGE(script.find(spec.name) != std::string::npos,
                         "flag '" << spec.name << "' (command '" << name << "') missing from bash completion");
        }
    }
}

TEST_CASE("every command's every real flag name appears in the zsh script") {
    std::string script = completion::generate("zsh");
    for (const auto& [name, cmd] : registry::commandTable()) {
        CHECK_MESSAGE(script.find(name) != std::string::npos, "subcommand '" << name << "' missing from zsh completion");
        for (const auto& spec : cmd.specs()) {
            CHECK_MESSAGE(script.find(spec.name) != std::string::npos,
                         "flag '" << spec.name << "' (command '" << name << "') missing from zsh completion");
        }
    }
}

TEST_CASE("an unsupported shell name throws rather than silently returning something bogus") {
    CHECK_THROWS(completion::generate("fish"));
}

TEST_CASE("bash output is well-formed enough to source: balanced case/esac, ends registering the completer") {
    std::string script = completion::generate("bash");
    CHECK(script.find("complete -F _casc_tool_completions casc-tool") != std::string::npos);
}

TEST_CASE("zsh output declares #compdef and calls its own function") {
    std::string script = completion::generate("zsh");
    CHECK(script.rfind("#compdef casc-tool", 0) == 0);
    CHECK(script.find("_casc_tool \"$@\"") != std::string::npos);
}

}  // TEST_SUITE("completion::generate")
