#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cli.hpp"

// The one place that lists every subcommand. main.cpp dispatches through
// this table instead of keeping its own copy, and completion.cpp reads the
// same table (name + real OptionSpec list) to generate shell completions --
// two consumers, one source of truth, so neither can silently drift from
// the other the way a hand-duplicated flag list would.
namespace registry {

struct Command {
    std::function<int(const std::vector<std::string>&)> run;
    std::function<void()> help;
    std::function<std::vector<cli::OptionSpec>()> specs;
    std::string oneLine;
};

const std::map<std::string, Command>& commandTable();

}  // namespace registry
