#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "listfile.hpp"
#include "rootmanifest.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

// Seeding is hash-bound on a local disk, not CPU-bound; 8 readers saturated
// a single ZFS pool in practice without starving the desktop. Not a flag
// until a second machine shows a different sweet spot.
constexpr unsigned kSeedThreads = 8;

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--label", true, "<name>",
                 "Build label. Default: the Version column of .build.info for --product (e.g. 12.1.0.69933)"});
    s.push_back({"--tag", true, "<name>",
                 "Point this tag at the build. Default by --product: wow -> live, wowt -> ptr, wow_beta -> beta"});
    s.push_back({"--seed-from", true, "<dir>",
                 "An existing export tree (files at their listfile paths): any file whose md5 matches the "
                 "needed CKey is cloned in instead of extracted. Verified, so a wrong file is just skipped"});
    s.push_back({"--mask", true, "<glob>",
                 "Only fetch objects for FDIDs whose listfile path matches (the manifest still records the "
                 "whole build; the rest is listed as not-fetched in missing.tsv)"});
    return s;
}

const char* kUsage = "casc-tool snapshot <store-dir> [options]";

// Locale the store serves (DESIGN.md: enUS default, decided 2026-10-10).
constexpr uint32_t kStoreLocale = CASC_LOCALE_ENUS;

// IN, non-critical (only names the build): .build.info is '|'-separated with a
// "Name!TYPE:size" header row; picks the Active=1 row for `product`.
std::string versionFromBuildInfo(const fs::path& install, const std::string& product, std::string* error) {
    std::ifstream in(install / ".build.info");
    std::string header;
    if (!in || !std::getline(in, header)) {
        *error = "couldn't read " + (install / ".build.info").string();
        return "";
    }
    auto split = [](const std::string& line) {
        std::vector<std::string> cols;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, '|');) cols.push_back(c);
        return cols;
    };
    std::map<std::string, size_t> col;
    auto names = split(header);
    for (size_t i = 0; i < names.size(); i++) col[names[i].substr(0, names[i].find('!'))] = i;
    for (const char* need : {"Active", "Version", "Product"})
        if (!col.count(need)) {
            *error = ".build.info header has no '" + std::string(need) + "' column: " + header;
            return "";
        }
    for (std::string line; std::getline(in, line);) {
        auto cells = split(line);
        if (cells.size() < names.size()) continue;
        if (cells[col["Active"]] == "1" && cells[col["Product"]] == product) return cells[col["Version"]];
    }
    *error = ".build.info has no active row for product '" + product + "' (pass --label)";
    return "";
}

std::string defaultTag(const std::string& product) {
    static const std::map<std::string, std::string> tags = {{"wow", "live"}, {"wowt", "ptr"}, {"wow_beta", "beta"}};
    auto it = tags.find(product);
    return it == tags.end() ? "" : it->second;
}

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

bool writeText(const fs::path& path, const std::string& text) {
    fs::path staged = path.string() + ".tmp";
    std::ofstream out(staged, std::ios::binary);
    out << text;
    out.close();
    if (!out) return false;
    fs::rename(staged, path);
    return true;
}

std::set<uint32_t> fdidsMatching(HANDLE hStorage, const std::string& mask, const std::string& listFile) {
    std::set<uint32_t> out;
    CASC_FIND_DATA fd{};
    storage::FindHandle hFind(CascFindFirstFile(hStorage, mask.c_str(), &fd, listFile.c_str()));
    if (!hFind || fd.dwFileDataId == CASC_INVALID_ID) return out;
    do {
        if (fd.dwFileDataId != CASC_INVALID_ID) out.insert(fd.dwFileDataId);
    } while (CascFindNextFile(hFind.get(), &fd));
    return out;
}

void progress(const char* phase, size_t done, size_t total) {
    std::fprintf(stderr, "\r%s: %zu/%zu", phase, done, total);
    std::fflush(stderr);
}

}  // namespace

std::vector<cli::OptionSpec> snapshotSpecs() { return specs(); }

void helpSnapshot() {
    std::printf(
        "casc-tool snapshot -- record the installed build into a build history store\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <store-dir>   the store (created if missing); see DESIGN.md \"Store layout\"\n"
        "\n"
        "Writes builds/<label>/ (the build's ROOT, manifest.tsv, missing.tsv,\n"
        "meta.json), then adds every enUS file the store doesn't have yet as\n"
        "objects/../<ckey>. Only new content costs disk: a hotfix build adds a\n"
        "few hundred files. Re-running on the same build resumes; it never\n"
        "rewrites an object.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool snapshot /data/wow_store --storage /games/wow/Data --listfile lf.csv --keys WoW.txt\n"
        "  casc-tool snapshot /data/wow_store ... --seed-from /data/wow_export   # first run: clone, don't extract\n");
}

int runSnapshot(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(1, 1, kUsage);
    store::Store st(args.positionals()[0]);
    auto opts = storage::fromArgs(args);
    std::string product = opts.product.value_or("wow");
    std::string listFile = storage::listFileFromArgs(args);
    std::string error;

    std::string label = args.optionOr("--label", "");
    if (label.empty()) label = versionFromBuildInfo(storage::installRoot(opts.path), product, &error);
    if (label.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::string tag = args.optionOr("--tag", defaultTag(product));
    auto seedFrom = args.option("--seed-from");
    auto mask = args.option("--mask");

    storage::StorageHandle hStorage;
    if (!storage::open(opts, hStorage)) return 1;

    std::vector<uint8_t> rootBytes;
    if (!storage::readFile(hStorage.get(), "ROOT", listFile, rootBytes, &error)) {
        std::fprintf(stderr, "error: reading ROOT: %s\n", error.c_str());
        return 1;
    }
    auto rows = rootmanifest::parse(rootBytes, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: ROOT: %s\n", error.c_str());
        return 1;
    }

    fs::path buildDir = st.buildDir(label);
    fs::path rootPath = buildDir / "ROOT";
    if (fs::exists(rootPath)) {
        std::ifstream in(rootPath, std::ios::binary);
        std::vector<uint8_t> existing{std::istreambuf_iterator<char>(in), {}};
        if (existing != rootBytes) {
            std::fprintf(stderr, "error: %s already holds a different ROOT -- pick another --label\n",
                         buildDir.string().c_str());
            return 1;
        }
        std::fprintf(stderr, "resuming build %s\n", label.c_str());
    } else {
        fs::create_directories(buildDir);
        if (!writeText(rootPath, std::string(rootBytes.begin(), rootBytes.end()))) {
            std::fprintf(stderr, "error: couldn't write %s\n", rootPath.string().c_str());
            return 1;
        }
    }

    // Derived, for grep/join without casc-tool (DESIGN.md: "Everything is plain files").
    {
        std::vector<const rootmanifest::Row*> sorted;
        for (const auto& r : rows) sorted.push_back(&r);
        std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->fdid < b->fdid; });
        std::string tsv = "fdid\tlocale\tcontent_flags\tckey\n";
        char line[96];
        for (auto* r : sorted) {
            std::snprintf(line, sizeof(line), "%u\t%08x\t%08x\t", r->fdid, r->localeFlags, r->contentFlags);
            tsv += line + rootmanifest::hex(r->ckey) + "\n";
        }
        writeText(buildDir / "manifest.tsv", tsv);
    }

    auto selected = rootmanifest::select(rows, kStoreLocale);
    std::set<uint32_t> inScope;
    if (mask) {
        inScope = fdidsMatching(hStorage.get(), *mask, listFile);
        std::fprintf(stderr, "--mask '%s' matched %zu FDIDs\n", mask->c_str(), inScope.size());
    }

    // One fetch per distinct CKey; remember one FDID per CKey for naming/seeding.
    std::map<store::CKey, uint32_t> wanted;
    for (const auto& [fdid, ckey] : selected) {
        if (mask && !inScope.count(fdid)) continue;
        if (!st.hasObject(ckey)) wanted.emplace(ckey, fdid);
    }
    std::fprintf(stderr, "build %s: %zu FDIDs (enUS), %zu objects to add\n", label.c_str(), selected.size(),
                 wanted.size());

    std::map<store::CKey, std::string> failures;
    std::mutex failuresMutex;

    if (seedFrom && !wanted.empty()) {
        std::string listError;
        auto names = listfile::load(listFile, &listError);
        if (!listError.empty()) {
            std::fprintf(stderr, "error: --seed-from needs names: %s\n", listError.c_str());
            return 1;
        }
        std::vector<std::pair<store::CKey, fs::path>> jobs;
        for (const auto& [ckey, fdid] : wanted) {
            auto n = names.find(fdid);
            if (n == names.end()) continue;
            std::string rel = storage::sanitizeRelativePath(n->second);
            fs::path candidate = fs::path(*seedFrom) / rel;
            std::error_code ec;
            if (!rel.empty() && fs::is_regular_file(candidate, ec)) jobs.emplace_back(ckey, candidate);
        }
        std::atomic<size_t> next{0}, done{0}, seeded{0};
        auto worker = [&] {
            for (size_t i; (i = next++) < jobs.size();) {
                std::string e;
                if (st.putFile(jobs[i].first, jobs[i].second, &e)) seeded++;
                if (++done % 1000 == 0) progress("seeding", done, jobs.size());
            }
        };
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < kSeedThreads; t++) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
        progress("seeding", done, jobs.size());
        std::fprintf(stderr, "\nseeded %zu of %zu candidates from %s (mismatches are stale copies, fetched below)\n",
                     seeded.load(), jobs.size(), seedFrom->c_str());
    }

    size_t fetched = 0, attempted = 0, remaining = 0;
    for (const auto& [ckey, fdid] : wanted)
        if (!st.hasObject(ckey)) remaining++;
    for (const auto& [ckey, fdid] : wanted) {
        if (st.hasObject(ckey)) continue;
        attempted++;
        storage::FileHandle hFile;
        std::vector<uint8_t> data;
        std::string e;
        bool ok = CascOpenFile(hStorage.get(), ckey.data(), 0, CASC_OPEN_BY_CKEY, hFile.out());
        ULONGLONG size = 0;
        DWORD read = 0;
        if (ok) ok = CascGetFileSize64(hFile.get(), &size);
        if (ok) {
            data.resize(size);
            ok = CascReadFile(hFile.get(), data.data(), static_cast<DWORD>(size), &read) && read == size;
        }
        if (!ok) {
            DWORD code = GetCascError();
            ULONGLONG keyName = 0;
            char buf[48];
            if (code == ERROR_FILE_ENCRYPTED && CascGetNotFoundEncryptionKey(hStorage.get(), &keyName))
                std::snprintf(buf, sizeof(buf), "encrypted:%016llX", static_cast<unsigned long long>(keyName));
            else
                std::snprintf(buf, sizeof(buf), "unreadable:%u", code);
            failures[ckey] = buf;
        } else if (!st.putBytes(ckey, data.data(), data.size(), &e)) {
            failures[ckey] = "rejected:" + e;
        } else {
            fetched++;
        }
        if (attempted % 1000 == 0) progress("extracting", attempted, remaining);
    }
    if (attempted) {
        progress("extracting", attempted, remaining);
        std::fprintf(stderr, "\n");
    }

    // missing.tsv: every selected FDID without an object, and why. Rewritten
    // whole each run -- it's derived from the store's current state.
    std::string missingTsv = "fdid\tckey\treason\n";
    size_t missingCount = 0;
    std::vector<std::pair<uint32_t, store::CKey>> ordered(selected.begin(), selected.end());
    std::sort(ordered.begin(), ordered.end());
    for (const auto& [fdid, ckey] : ordered) {
        if (st.hasObject(ckey)) continue;
        auto f = failures.find(ckey);
        std::string reason = f != failures.end() ? f->second : "not-fetched";
        missingTsv += std::to_string(fdid) + "\t" + rootmanifest::hex(ckey) + "\t" + reason + "\n";
        missingCount++;
    }
    writeText(buildDir / "missing.tsv", missingTsv);

    std::string meta = "{\n  \"label\": \"" + label + "\",\n  \"product\": \"" + product +
                       "\",\n  \"locale\": \"enUS\",\n  \"fdids\": " + std::to_string(selected.size()) +
                       ",\n  \"missing\": " + std::to_string(missingCount) + ",\n  \"snapshot_at\": \"" + nowIso() +
                       "\"\n}\n";
    writeText(buildDir / "meta.json", meta);

    if (!tag.empty()) st.setTag(tag, label);
    std::fprintf(stderr, "build %s: extracted %zu, %zu FDIDs missing (see missing.tsv)%s\n", label.c_str(), fetched,
                 missingCount, tag.empty() ? "" : (", tagged " + tag).c_str());
    return 0;
}

}  // namespace commands
