#include <cstdio>
#include <string>
#include <vector>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "listfile.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    return {
        {"--format", true, "<text|csv|json>", "Output format. Default: text"},
    };
}

const char* kUsage = "casc-tool diff <listfile-a> <listfile-b> [options]";

}  // namespace

void helpDiff() {
    std::printf(
        "casc-tool diff -- compare two listfile snapshots\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <listfile-a>   older snapshot, e.g. a listfile.csv you downloaded\n"
        "                 a while ago and kept around\n"
        "  <listfile-b>   newer snapshot, e.g. a freshly downloaded one\n"
        "\n"
        "Reports FileDataIDs added, removed, or renamed between the two -- e.g.\n"
        "\"what did this patch actually add,\" if you archive listfiles somewhere\n"
        "before updating them. Doesn't touch any CASC storage; this is pure\n"
        "listfile-to-listfile comparison.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "example:\n"
        "  casc-tool diff listfile.csv.old listfile.csv\n");
}

int runDiff(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(2, 2, kUsage);
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "csv" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text, csv, or json (got '%s')\n", fmt.c_str());
        return 2;
    }

    std::string error;
    auto a = listfile::load(args.positionals()[0], &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    auto b = listfile::load(args.positionals()[1], &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    auto changes = listfile::diff(a, b);

    if (fmt == "csv") {
        std::printf("fdid,change,old_name,new_name\n");
        for (const auto& c : changes) {
            std::printf("%u,%c,%s,%s\n", c.id, c.kind, format::csvEscape(c.oldName).c_str(),
                        format::csvEscape(c.newName).c_str());
        }
    } else if (fmt == "json") {
        std::printf("[\n");
        for (size_t i = 0; i < changes.size(); i++) {
            const auto& c = changes[i];
            std::printf("%s{\"fdid\":%u,\"change\":\"%c\",\"old_name\":\"%s\",\"new_name\":\"%s\"}\n",
                        i > 0 ? "," : "", c.id, c.kind, format::jsonEscape(c.oldName).c_str(),
                        format::jsonEscape(c.newName).c_str());
        }
        std::printf("]\n");
    } else {
        for (const auto& c : changes) {
            if (c.kind == 'A') std::printf("+ %u %s\n", c.id, c.newName.c_str());
            else if (c.kind == 'R') std::printf("- %u %s\n", c.id, c.oldName.c_str());
            else std::printf("~ %u %s -> %s\n", c.id, c.oldName.c_str(), c.newName.c_str());
        }
    }

    unsigned long long added = 0, removed = 0, renamed = 0;
    for (const auto& c : changes) {
        if (c.kind == 'A') added++;
        else if (c.kind == 'R') removed++;
        else renamed++;
    }
    std::fprintf(stderr, "%llu added, %llu removed, %llu renamed\n", added, removed, renamed);
    return 0;
}

}  // namespace commands
