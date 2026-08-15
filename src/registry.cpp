#include "registry.hpp"

#include "commands.hpp"

namespace registry {

const std::map<std::string, Command>& commandTable() {
    static const std::map<std::string, Command> table = {
        {"list", {commands::runList, commands::helpList, commands::listSpecs, "list files in a CASC storage"}},
        {"info", {commands::runInfo, commands::helpInfo, commands::infoSpecs, "show metadata for one file"}},
        {"extract",
         {commands::runExtract, commands::helpExtract, commands::extractSpecs, "extract a single file to disk"}},
        {"extract-batch",
         {commands::runExtractBatch, commands::helpExtractBatch, commands::extractBatchSpecs,
          "bulk-extract matching files to a directory"}},
        {"diff",
         {commands::runDiff, commands::helpDiff, commands::diffSpecs, "compare two listfile snapshots"}},
    };
    return table;
}

}  // namespace registry
