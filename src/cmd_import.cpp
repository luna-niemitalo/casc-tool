#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "ingest.hpp"
#include "rootmanifest.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

// Same locale snapshot and mount use (cmd_snapshot.cpp kStoreLocale).
constexpr uint32_t kStoreLocale = CASC_LOCALE_ENUS;

std::vector<cli::OptionSpec> specs() {
    return {
        {"--label", true, "<name>", "Build label (required), e.g. 12.0.7.68453 or legacy-wow_export"},
        {"--root", true, "<file>",
         "The tree's own ROOT manifest: import as that build, adopting files whose content it lists. "
         "Without --root the tree is a path build: every file is recorded under its own path"},
        {"--tag", true, "<name>", "Point this tag at the imported build (e.g. old, legacy)"},
        {"--move", false, "",
         "Move adopted files into the store instead of cloning them (tree must be on the store's filesystem)"},
        {"--drop-duplicates", false, "",
         "Delete tree files whose content the store already holds (hash-verified, so nothing is lost)"},
        {"--dry-run", false, "", "Hash and classify everything, report, change nothing"},
    };
}

const char* kUsage = "casc-tool import <store-dir> <tree> --label <name> [options]";

// What one file of the tree turned out to be.
enum class Kind : uint8_t {
    Unread,     // couldn't be hashed (reported, left alone)
    Adopted,    // new content, now an object
    Stored,     // content the store already had (and, with --root, the ROOT lists)
    Repeat,     // same content as another file adopted in this run
    Foreign,    // --root only: content this ROOT doesn't list (stale, other build)
};

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

}  // namespace

std::vector<cli::OptionSpec> importSpecs() { return specs(); }

void helpImport() {
    std::printf(
        "casc-tool import -- ingest a tree of exported files into a build history store\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <store-dir>   the store (see DESIGN.md)\n"
        "  <tree>        an export tree, or what's left of one\n"
        "\n"
        "Hashes every file. With --root, the tree is that ROOT's build: files whose\n"
        "content the ROOT lists become objects, everything else is left in the tree\n"
        "for a second, path-build import. Without --root, every file is recorded at\n"
        "its own path (builds/<label>/files.tsv) -- for content of no known build.\n"
        "Either way identical content is stored once.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool import /data/store /data/old_export --label 12.0.7.68453 --root /data/old_export/ROOT --dry-run\n"
        "  casc-tool import /data/store /data/store/legacy --label legacy-wow_export --tag legacy --move\n");
}

int runImport(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(2, 2, kUsage);
    store::Store st(args.positionals()[0]);
    fs::path tree = args.positionals()[1];
    std::string label = args.optionOr("--label", "");
    auto rootFile = args.option("--root");
    bool dryRun = args.flag("--dry-run");
    bool dropDuplicates = args.flag("--drop-duplicates");
    auto mode = args.flag("--move") ? store::Store::Adopt::Move : store::Store::Adopt::Clone;

    if (label.empty() || label.find('/') != std::string::npos || label[0] == '.') {
        std::fprintf(stderr, "error: --label is required and must be a plain name (got '%s')\n", label.c_str());
        return 2;
    }
    if (!fs::is_directory(tree)) {
        std::fprintf(stderr, "error: %s is not a directory\n", tree.string().c_str());
        return 1;
    }
    if (ingest::containsStore(tree, st.root())) {
        std::fprintf(stderr, "error: %s contains the store %s -- import a subdirectory instead\n",
                     tree.string().c_str(), st.root().string().c_str());
        return 2;
    }

    std::string error;
    std::vector<uint8_t> rootBytes;
    std::vector<rootmanifest::Row> rows;
    std::set<store::CKey> listed;  // every CKey the ROOT names, any locale
    if (rootFile) {
        std::ifstream in(*rootFile, std::ios::binary);
        rootBytes.assign(std::istreambuf_iterator<char>(in), {});
        rows = rootmanifest::parse(rootBytes, &error);
        if (!error.empty()) {
            std::fprintf(stderr, "error: %s: %s\n", rootFile->c_str(), error.c_str());
            return 1;
        }
        for (const auto& r : rows) listed.insert(r.ckey);
    }

    uint64_t totalBytes = 0;
    auto files = ingest::listFiles(tree, &totalBytes, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s %s as %s build %s: %zu files, %.1f GB to hash%s\n", dryRun ? "classifying" : "importing",
                 tree.string().c_str(), rootFile ? "ROOT" : "path", label.c_str(), files.size(),
                 double(totalBytes) / 1e9, dryRun ? " (dry run)" : "");

    std::vector<Kind> kinds(files.size(), Kind::Unread);
    std::vector<store::CKey> keys(files.size());
    std::set<store::CKey> claimed;
    std::mutex claimedMutex;
    ingest::parallelFor(files.size(), "hashing", [&](size_t i) {
        auto classify = [&](const store::CKey& key) {
            // Foreign before Stored: content this ROOT doesn't list stays in the tree for a
            // path import even if stored, so --drop-duplicates never loses where it was.
            if (rootFile && !listed.count(key)) return Kind::Foreign;
            if (st.hasObject(key)) return Kind::Stored;
            std::lock_guard<std::mutex> lock(claimedMutex);
            return claimed.insert(key).second ? Kind::Adopted : Kind::Repeat;
        };
        auto want = [&](const store::CKey& key) {
            keys[i] = key;
            kinds[i] = classify(key);
            return kinds[i] == Kind::Adopted && !dryRun;
        };
        std::string e;
        bool adopted = st.adoptFile(files[i], mode, want, &e).has_value();
        if (!dryRun && kinds[i] == Kind::Adopted && !adopted) {
            std::fprintf(stderr, "\nwarning: %s: %s\n", files[i].string().c_str(), e.c_str());
            kinds[i] = Kind::Unread;
        }
    });

    // Tally before any deletion, so the report describes the tree as found.
    struct Tally { size_t files = 0; uint64_t bytes = 0; };
    std::map<Kind, Tally> tally;
    std::vector<uint64_t> sizes(files.size());
    for (size_t i = 0; i < files.size(); i++) {
        std::error_code ec;
        sizes[i] = kinds[i] == Kind::Adopted && mode == store::Store::Adopt::Move && !dryRun
                       ? fs::file_size(st.objectPath(keys[i]), ec)
                       : fs::file_size(files[i], ec);
        tally[kinds[i]].files++;
        tally[kinds[i]].bytes += sizes[i];
    }

    fs::path buildDir = st.buildDir(label);
    size_t missingCount = 0;
    if (!dryRun) {
        if (rootFile) {
            if (!st.recordRoot(label, rootBytes, rows, &error)) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 1;
            }
            auto selected = rootmanifest::select(rows, kStoreLocale);
            std::vector<std::pair<uint32_t, store::CKey>> ordered(selected.begin(), selected.end());
            std::sort(ordered.begin(), ordered.end());
            std::string missing = "fdid\tckey\treason\n";
            for (const auto& [fdid, ckey] : ordered) {
                if (st.hasObject(ckey)) continue;
                missing += std::to_string(fdid) + "\t" + rootmanifest::hex(ckey) + "\tnot-in-export\n";
                missingCount++;
            }
            store::writeTextAtomic(buildDir / "missing.tsv", missing);
        } else {
            // The path build's whole record: each file's path in the tree and its content.
            fs::create_directories(buildDir);
            std::vector<std::pair<std::string, store::CKey>> entries;
            for (size_t i = 0; i < files.size(); i++)
                if (kinds[i] != Kind::Unread) entries.emplace_back(fs::relative(files[i], tree).string(), keys[i]);
            std::sort(entries.begin(), entries.end());
            std::string tsv = "path\tckey\n";
            for (const auto& [path, key] : entries) tsv += path + "\t" + rootmanifest::hex(key) + "\n";
            store::writeTextAtomic(buildDir / "files.tsv", tsv);
        }
        std::string meta = "{\n  \"label\": \"" + label + "\",\n  \"kind\": \"" + (rootFile ? "root" : "paths") +
                           "\",\n  \"source\": \"import:" + fs::absolute(tree).string() +
                           "\",\n  \"locale\": \"enUS\",\n  \"missing\": " + std::to_string(missingCount) +
                           ",\n  \"imported_at\": \"" + nowIso() + "\"\n}\n";
        store::writeTextAtomic(buildDir / "meta.json", meta);
        if (auto tag = args.option("--tag")) st.setTag(*tag, label);
    }

    // Only after every adoption has committed: a duplicate is deleted only if
    // its content is verifiably an object right now (invariant 1).
    size_t dropped = 0;
    uint64_t droppedBytes = 0;
    if (dropDuplicates && !dryRun) {
        for (size_t i = 0; i < files.size(); i++) {
            bool redundant = kinds[i] == Kind::Stored || kinds[i] == Kind::Repeat;
            std::error_code ec;
            if (redundant && st.hasObject(keys[i]) && fs::remove(files[i], ec)) {
                dropped++;
                droppedBytes += sizes[i];
            }
        }
    }

    auto line = [&](const char* name, Kind k, const char* what) {
        std::fprintf(stderr, "  %-9s %8zu files %8.2f GB  %s\n", name, tally[k].files, double(tally[k].bytes) / 1e9,
                     what);
    };
    std::fprintf(stderr, "%s:\n", dryRun ? "would import" : "imported");
    line("adopted", Kind::Adopted, "new content, stored once");
    line("stored", Kind::Stored, "content the store already had");
    line("repeat", Kind::Repeat, "same content as another file in this tree");
    if (rootFile) line("foreign", Kind::Foreign, "not listed by this ROOT -- left in the tree");
    line("unread", Kind::Unread, "couldn't be hashed or stored -- left in the tree");
    if (rootFile && !dryRun)
        std::fprintf(stderr, "  %zu enUS FDIDs of this build have no object (missing.tsv)\n", missingCount);
    if (dropDuplicates && !dryRun)
        std::fprintf(stderr, "dropped %zu redundant files (%.2f GB) from the tree\n", dropped, double(droppedBytes) / 1e9);
    return 0;
}

}  // namespace commands
