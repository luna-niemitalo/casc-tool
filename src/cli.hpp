#pragma once

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Minimal, dependency-free option parsing. Deliberately small: this tool has
// a handful of commands with a handful of flags each, not enough surface to
// justify pulling in an argument-parsing library.
namespace cli {

// Thrown for anything the user typed wrong (unknown flag, missing value,
// wrong number of positionals). Callers catch this, print .what() plus a
// pointer to --help, and exit non-zero -- never a raw crash on bad input.
class ArgError : public std::runtime_error {
public:
    explicit ArgError(const std::string& msg) : std::runtime_error(msg) {}
};

struct OptionSpec {
    std::string name;       // e.g. "--storage", including the leading "--"
    bool takesValue;        // true: "--name value" or "--name=value"; false: bare flag
    std::string valueHint;  // e.g. "<path>", shown in help; empty for bare flags
    std::string help;       // one-line description, shown in help
};

// Parses argv (already past the subcommand name) against a fixed set of
// recognized options. Anything not matching a known "--flag" is treated as
// positional; an unrecognized "--flag" is a hard error (typos should be
// caught immediately, not silently ignored).
class Args {
public:
    Args(const std::vector<std::string>& argv, const std::vector<OptionSpec>& specs);

    bool flag(const std::string& name) const;
    std::optional<std::string> option(const std::string& name) const;
    std::string optionOr(const std::string& name, const std::string& fallback) const;
    const std::vector<std::string>& positionals() const { return positionals_; }

    // Throws ArgError if the positional count isn't in [minCount, maxCount].
    // maxCount < 0 means unbounded.
    void requirePositionals(size_t minCount, long maxCount, const std::string& usage) const;

private:
    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
    std::vector<std::string> positionals_;
};

// Prints a two-column "  --flag <value>   description" table, used by every
// command's --help output so they all look the same.
void printOptionTable(const std::vector<OptionSpec>& specs);

}  // namespace cli
