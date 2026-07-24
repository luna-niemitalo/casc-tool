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
    std::string limitStr = args.optionOr("--limit", "100");
    long limit = 0;
    bool limitOk = true;
    try {
        size_t consumed = 0;
        limit = std::stol(limitStr, &consumed);
        limitOk = consumed == limitStr.size();
    } catch (const std::exception&) {
        limitOk = false;
    }
    if (!limitOk || limit < 0) {
        std::fprintf(stderr, "error: --limit must be a whole number >= 0 (got '%s')\n", limitStr.c_str());
        return 2;
    }

    std::string listFileError;
    if (!storage::checkListFileExists(listFile, &listFileError)) {
        std::fprintf(stderr, "error: %s\n", listFileError.c_str());
        return 1;
    }

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    CASC_FIND_DATA fd{};
    storage::FindHandle hFind(CascFindFirstFile(hStorage.get(), mask.c_str(), &fd, listFile.c_str()));
    // A literal (non-wildcard) mask matching nothing has been observed to
    // still return a non-null handle with fd left holding
    // CASC_INVALID_ID/garbage -- see storage::openFile's comment. Treat
    // that the same as "no files matched".
    if (!hFind || fd.dwFileDataId == CASC_INVALID_ID) {
        std::fprintf(stderr, "error: no files matched '%s'\n", mask.c_str());
        return 1;
    }

    unsigned long long scanned = 0, matched = 0, shown = 0, skippedNoId = 0;
    if (fmt == "csv") std::printf("fdid,size,resolved,name\n");
    if (fmt == "json") std::printf("[\n");

    do {
        scanned++;
        bool resolved = fd.NameType == CascNameFull;
        bool hasFileDataId = fd.dwFileDataId != CASC_INVALID_ID;

        // Entries with no FileDataID at all are CKey/EKey-only CASC
        // components (patch/build metadata, not game assets) -- they
        // always report as unresolved (no NameType == CascNameFull is
        // possible without an ID) but they can never be named or opened by
        // info/extract either, so they don't belong in the "--unresolved
        // FileDataIDs needing a listfile entry" worklist. Keep them out of
        // it (and out of its matched/shown counts) instead of presenting
        // them as if they were unnamed files (see FAILURES.md #2/#3).
        if (unresolvedOnly && !hasFileDataId) {
            skippedNoId++;
            continue;
        }
        if (unresolvedOnly && resolved) continue;
        matched++;

        bool willPrint = limit == 0 || static_cast<long>(shown) < limit;
        if (!willPrint) continue;
        shown++;

        if (fmt == "text") {
            std::string fdidStr = hasFileDataId ? std::to_string(fd.dwFileDataId) : "-";
            std::printf("  [%s] fdid=%s size=%llu %s\n", resolved ? "name" : "id  ", fdidStr.c_str(),
                        static_cast<unsigned long long>(fd.FileSize), fd.szFileName);
        } else if (fmt == "csv") {
            std::string fdidStr = hasFileDataId ? std::to_string(fd.dwFileDataId) : "";
            std::printf("%s,%llu,%d,%s\n", fdidStr.c_str(), static_cast<unsigned long long>(fd.FileSize),
                        resolved ? 1 : 0, format::csvEscape(fd.szFileName).c_str());
        } else {
            std::string fdidJson = hasFileDataId ? std::to_string(fd.dwFileDataId) : "null";
            std::printf("%s{\"fdid\":%s,\"size\":%llu,\"resolved\":%s,\"name\":\"%s\"}\n", shown > 1 ? "," : "",
                        fdidJson.c_str(), static_cast<unsigned long long>(fd.FileSize), resolved ? "true" : "false",
                        format::jsonEscape(fd.szFileName).c_str());
        }
    } while (CascFindNextFile(hFind.get(), &fd));

    if (fmt == "json") std::printf("]\n");
    if (fmt == "text" && limit != 0 && matched > shown) {
        std::printf("  ... (%llu more; rerun with --limit 0 to see all)\n", matched - shown);
    }

    std::fprintf(stderr, "scanned %llu entries, %llu matched, %llu shown", scanned, matched, shown);
    if (skippedNoId > 0) {
        std::fprintf(stderr, ", %llu skipped (no FileDataID -- not a nameable file)", skippedNoId);
    }
    std::fprintf(stderr, "\n");
    return 0;
}

}  // namespace commands
