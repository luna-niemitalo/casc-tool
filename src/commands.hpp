#pragma once

#include <string>
#include <vector>

// One run()/help() pair per subcommand. main.cpp dispatches to these by
// name; each file below owns its own --help text and argument handling so
// the command list can grow without main.cpp needing to know the details.
namespace commands {

int runList(const std::vector<std::string>& args);
void helpList();

int runInfo(const std::vector<std::string>& args);
void helpInfo();

int runExtract(const std::vector<std::string>& args);
void helpExtract();

int runExtractBatch(const std::vector<std::string>& args);
void helpExtractBatch();

int runDiff(const std::vector<std::string>& args);
void helpDiff();

}  // namespace commands
