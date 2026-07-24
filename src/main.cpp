#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"

namespace {

struct Command {
    std::function<int(const std::vector<std::string>&)> run;
    std::function<void()> help;
    std::string oneLine;
};

const std::map<std::string, Command>& commandTable() {
    static const std::map<std::string, Command> table = {
        {"list", {commands::runList, commands::helpList, "list files in a CASC storage"}},
        {"info", {commands::runInfo, commands::helpInfo, "show metadata for one file"}},
        {"extract", {commands::runExtract, commands::helpExtract, "extract a single file to disk"}},
        {"extract-batch",
         {commands::runExtractBatch, commands::helpExtractBatch, "bulk-extract matching files to a directory"}},
        {"diff", {commands::runDiff, commands::helpDiff, "compare two listfile snapshots"}},
    };
    return table;
}

bool wantsHelp(const std::vector<std::string>& args) {
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") return true;
    }
    return false;
}

void printTopLevelHelp() {
    std::printf(
        "casc-tool -- browse and extract files from a World of Warcraft CASC storage\n"
        "(wraps CascLib %s: https://github.com/ladislav-zezula/CascLib)\n"
        "\n"
        "usage: casc-tool <command> [arguments] [options]\n"
        "       casc-tool --help              show this message\n"
        "       casc-tool <command> --help    show help for one command\n"
        "       casc-tool --version           show version info\n"
        "\n"
        "commands:\n",
        CASCLIB_VERSION_STRING);
    for (const auto& [name, cmd] : commandTable()) {
        std::printf("  %-16s %s\n", name.c_str(), cmd.oneLine.c_str());
    }
    std::printf(
        "\n"
        "quick start:\n"
        "  1. Point --storage at a directory containing .build.info (your WoW\n"
        "     install, or a read-only copy of it -- see README.md for why a copy\n"
        "     is a good idea).\n"
        "  2. Get a listfile (FileDataID <-> path mapping) from\n"
        "     https://github.com/wowdev/wow-listfile/releases -- download\n"
        "     community-listfile.csv and pass it as --listfile.\n"
        "  3. List what's in it:  casc-tool list --limit 10\n"
        "  4. Pull one file out:  casc-tool extract <path-or-id>\n"
        "\n"
        "--storage defaults to '.' and --listfile to 'listfile.csv', both in the\n"
        "current directory -- pass --storage/--listfile explicitly to use\n"
        "anything else. See README.md for the full picture (what CASC is, how\n"
        "the listfile works, troubleshooting).\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printTopLevelHelp();
        return 0;
    }
    if (args[0] == "--version") {
        std::printf("casc-tool (CascLib %s)\n", CASCLIB_VERSION_STRING);
        return 0;
    }

    auto it = commandTable().find(args[0]);
    if (it == commandTable().end()) {
        std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].c_str());
        printTopLevelHelp();
        return 2;
    }

    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (wantsHelp(rest)) {
        it->second.help();
        return 0;
    }

    try {
        return it->second.run(rest);
    } catch (const cli::ArgError& e) {
        std::fprintf(stderr, "error: %s\n\nrun 'casc-tool %s --help' for usage\n", e.what(), args[0].c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
