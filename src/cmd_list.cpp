#include <cstdio>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "storage.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    std::vector<cli::OptionSpec> s = storage::commonOptionSpecs();
    s.push_back({"--unresolved-only", false, "",
                 "Only show entries with no name in --listfile (FileDataID-only) -- the actual "
                 "worklist for growing the community listfile after a patch"});
    s.push_back({"--format", true, "<text|csv|json>", "Output format. Default: text"});
    s.push_back({"--limit", true, "<n>", "Max rows to print, 0 = unlimited. Default: 100 (applies to every format)"});
    return s;
}

const char* kUsage = "casc-tool list [mask] [options]";

}  // namespace

void helpList() {
    std::printf(
        "casc-tool list -- list files in a CASC storage\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  [mask]   glob to match in-storage paths, e.g. 'character/*' or '*.m2'.\n"
        "           Default: '*' (everything)\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool list                          # everything, first 100 (text)\n"
        "  casc-tool list 'character/*' --limit 0  # all character files, no cap\n"
        "  casc-tool list --unresolved-only --format csv --limit 0 > unnamed.csv\n"
        "                                           # full worklist for the community listfile\n");
}

int runList(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(0, 1, kUsage);

    std::string mask = args.positionals().empty() ? "*" : args.positionals()[0];
    std::string listFile = storage::listFileFromArgs(args);
    bool unresolvedOnly = args.flag("--unresolved-only");
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "csv" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text, csv, or json (got '%s')\n", fmt.c_str());
        return 2;
    }
    long limit = std::stol(args.optionOr("--limit", "100"));

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    CASC_FIND_DATA fd;
    storage::FindHandle hFind(CascFindFirstFile(hStorage.get(), mask.c_str(), &fd, listFile.c_str()));
    if (!hFind) {
        std::fprintf(stderr, "error: no files matched '%s': %s\n", mask.c_str(),
                     storage::errorMessage(GetCascError()).c_str());
        return 1;
    }

    unsigned long long scanned = 0, matched = 0, shown = 0;
    if (fmt == "csv") std::printf("fdid,size,resolved,name\n");
    if (fmt == "json") std::printf("[\n");

    do {
        scanned++;
        bool resolved = fd.NameType == CascNameFull;
        if (unresolvedOnly && resolved) continue;
        matched++;

        bool willPrint = limit == 0 || static_cast<long>(shown) < limit;
        if (!willPrint) continue;
        shown++;

        if (fmt == "text") {
            std::printf("  [%s] fdid=%u size=%llu %s\n", resolved ? "name" : "id  ", fd.dwFileDataId,
                        static_cast<unsigned long long>(fd.FileSize), fd.szFileName);
        } else if (fmt == "csv") {
            std::printf("%u,%llu,%d,%s\n", fd.dwFileDataId, static_cast<unsigned long long>(fd.FileSize),
                        resolved ? 1 : 0, format::csvEscape(fd.szFileName).c_str());
        } else {
            std::printf("%s{\"fdid\":%u,\"size\":%llu,\"resolved\":%s,\"name\":\"%s\"}\n", shown > 1 ? "," : "",
                        fd.dwFileDataId, static_cast<unsigned long long>(fd.FileSize), resolved ? "true" : "false",
                        format::jsonEscape(fd.szFileName).c_str());
        }
    } while (CascFindNextFile(hFind.get(), &fd));

    if (fmt == "json") std::printf("]\n");
    if (fmt == "text" && limit != 0 && matched > shown) {
        std::printf("  ... (%llu more; rerun with --limit 0 to see all)\n", matched - shown);
    }

    std::fprintf(stderr, "scanned %llu entries, %llu matched, %llu shown\n", scanned, matched, shown);
    return 0;
}

}  // namespace commands
