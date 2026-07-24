#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    return {
        {"--format", true, "<text|csv|json>", "Output format. Default: text"},
    };
}

const char* kUsage = "casc-tool diff <listfile-a> <listfile-b> [options]";

// Parses a wow-listfile-style CSV: "FileDataId;FullFileName" per line.
// Deliberately doesn't reuse CascLib's own listfile parser here -- this
// command never opens a storage, and the format is simple enough (one
// split on the first ';') that pulling CascLib in for it would be backwards.
std::map<unsigned, std::string> loadListfile(const std::string& path, std::string* error) {
    std::map<unsigned, std::string> entries;
    std::ifstream in(path);
    if (!in) {
        *error = "couldn't open '" + path + "'";
        return entries;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto sep = line.find(';');
        if (sep == std::string::npos) continue;
        try {
            unsigned id = static_cast<unsigned>(std::stoul(line.substr(0, sep)));
            std::string name = line.substr(sep + 1);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            entries[id] = name;
        } catch (const std::exception&) {
            continue;  // malformed line, skip rather than abort a multi-million-line file
        }
    }
    return entries;
}

}  // namespace

void helpDiff() {
    std::printf(
        "casc-tool diff -- compare two listfile snapshots\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <listfile-a>   older snapshot, e.g. one of the\n"
        "                 listfile.csv.old_<timestamp> files scripts/update-listfile.nu\n"
        "                 leaves behind\n"
        "  <listfile-b>   newer snapshot, e.g. the current listfile.csv\n"
        "\n"
        "Reports FileDataIDs added, removed, or renamed between the two -- e.g.\n"
        "\"what did this patch actually add\" after running scripts/update-listfile.nu.\n"
        "Doesn't touch any CASC storage; this is pure listfile-to-listfile comparison.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "example:\n"
        "  casc-tool diff m2mod/mappings/listfile.csv.old_20260724T110000 \\\n"
        "                 m2mod/mappings/listfile.csv\n");
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
    auto a = loadListfile(args.positionals()[0], &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    auto b = loadListfile(args.positionals()[1], &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    struct Change {
        unsigned id;
        char kind;  // 'A' added, 'R' removed, 'C' changed (renamed)
        std::string oldName, newName;
    };
    std::vector<Change> changes;

    for (const auto& [id, name] : b) {
        auto it = a.find(id);
        if (it == a.end()) {
            changes.push_back({id, 'A', "", name});
        } else if (it->second != name) {
            changes.push_back({id, 'C', it->second, name});
        }
    }
    for (const auto& [id, name] : a) {
        if (b.find(id) == b.end()) changes.push_back({id, 'R', name, ""});
    }

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
