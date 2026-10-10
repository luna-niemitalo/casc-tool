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
        {"keys",
         {commands::runKeys, commands::helpKeys, commands::keysSpecs,
          "harvest TACT keys from game data and the hotfix cache"}},
        {"snapshot",
         {commands::runSnapshot, commands::helpSnapshot, commands::snapshotSpecs,
          "record the installed build into a build history store"}},
        {"mount",
         {commands::runMount, commands::helpMount, commands::mountSpecs,
          "browse every stored build as a read-only filesystem"}},
    };
    return table;
}

}  // namespace registry
