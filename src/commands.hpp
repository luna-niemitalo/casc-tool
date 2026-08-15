#pragma once

#include <string>
#include <vector>

#include "cli.hpp"

// One run()/help() pair per subcommand. main.cpp dispatches to these by
// name; each file below owns its own --help text and argument handling so
// the command list can grow without main.cpp needing to know the details.
//
// Each command also exposes its OptionSpec list (the same one its own
// --help and argument parsing already use) so a second consumer -- the
// shell-completion generator in completion.cpp -- can read the real,
// live flag surface instead of hand-listing flags a second time and
// silently drifting the next time one is added or renamed. See
// completion.cpp's own top comment for the full rationale (ported from
// husk's DESIGN.md "Shell completion generation" section).
namespace commands {

int runList(const std::vector<std::string>& args);
void helpList();
std::vector<cli::OptionSpec> listSpecs();

int runInfo(const std::vector<std::string>& args);
void helpInfo();
std::vector<cli::OptionSpec> infoSpecs();

int runExtract(const std::vector<std::string>& args);
void helpExtract();
std::vector<cli::OptionSpec> extractSpecs();

int runExtractBatch(const std::vector<std::string>& args);
void helpExtractBatch();
std::vector<cli::OptionSpec> extractBatchSpecs();

int runDiff(const std::vector<std::string>& args);
void helpDiff();
std::vector<cli::OptionSpec> diffSpecs();

}  // namespace commands
