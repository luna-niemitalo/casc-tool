#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "completion.hpp"
#include "registry.hpp"

namespace {

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
    for (const auto& [name, cmd] : registry::commandTable()) {
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
        "the listfile works, troubleshooting).\n"
        "\n"
        "shell completion: eval \"$(casc-tool --print-completion=bash)\" (or "
        "zsh), or copy completions/casc-tool.bash|.zsh from the repo.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    // Hidden `--print-completion=<bash|zsh>` support. No human reader --
    // its only consumers are `completions/*.regenerate` and the installed
    // completion script's own callback -- so it's deliberately absent from
    // --help, same call husk's DESIGN.md documents for the identical flag.
    if (args.size() == 1 && args[0].rfind("--print-completion=", 0) == 0) {
        std::string shell = args[0].substr(std::strlen("--print-completion="));
        try {
            std::fputs(completion::generate(shell).c_str(), stdout);
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
    }

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printTopLevelHelp();
        return 0;
    }
    if (args[0] == "--version") {
        std::printf("casc-tool (CascLib %s)\n", CASCLIB_VERSION_STRING);
        return 0;
    }

    auto it = registry::commandTable().find(args[0]);
    if (it == registry::commandTable().end()) {
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
