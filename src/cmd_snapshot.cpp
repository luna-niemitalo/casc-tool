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
#include "ingest.hpp"
#include "rootmanifest.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--label", true, "<name>",
                 "Build label. Default: the Version column of .build.info for --product (e.g. 12.1.0.69933)"});
    s.push_back({"--tag", true, "<name>",
                 "Point this tag at the build. Default by --product: wow -> live, wowt -> ptr, wow_beta -> beta"});
    s.push_back({"--seed-from", true, "<dir>",
                 "An existing export tree: every file in it is hashed, and any whose content is a needed CKey "
                 "is cloned in instead of extracted. Matching is by content, so the tree's naming doesn't matter"});
    s.push_back({"--seed-move", false, "",
                 "With --seed-from on the store's own filesystem: move matching files into the store instead of "
                 "cloning them (converts an export in place). Non-matching files are left where they are"});
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

// Reads a whole open file in chunks; *clean is how many bytes came through
// before any failure -- for an encrypted file, a lower bound on what's readable
// (CascLib's signal is only "this read hit an encrypted block").
DWORD readAll(HANDLE hFile, ULONGLONG size, std::vector<uint8_t>& out, size_t* clean) {
    constexpr DWORD kChunk = 1 << 20;
    out.assign(size, 0);
    *clean = 0;
    while (*clean < size) {
        DWORD want = DWORD(std::min<ULONGLONG>(kChunk, size - *clean)), got = 0;
        if (!CascReadFile(hFile, out.data() + *clean, want, &got)) return GetCascError();
        if (got == 0) return ERROR_HANDLE_EOF;
        *clean += got;
    }
    return ERROR_SUCCESS;
}

struct Partial {
    store::CKey object;  // md5 of the zero-filled bytes actually stored
    std::string keyName;
    double unreadAtLeast;
};

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
    auto seedMode = args.flag("--seed-move") ? store::Store::Adopt::Move : store::Store::Adopt::Clone;
    if (args.flag("--seed-move") && !seedFrom) {
        std::fprintf(stderr, "error: --seed-move needs --seed-from <dir>\n");
        return 2;
    }
    if (seedFrom && ingest::containsStore(*seedFrom, st.root())) {
        std::fprintf(stderr, "error: --seed-from %s contains the store %s -- seed from a subdirectory instead\n",
                     seedFrom->c_str(), st.root().string().c_str());
        return 2;
    }
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
    if (!st.recordRoot(label, rootBytes, rows, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
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
    std::map<store::CKey, Partial> partials;  // by real CKey
    std::mutex failuresMutex;

    if (seedFrom && !wanted.empty()) {
        // By content, not by path: every file under the seed tree is hashed and
        // adopted if it is a wanted CKey. Robust to the exporter's naming (an
        // older listfile, _unresolved/ conventions) at the cost of reading files
        // that turn out stale -- those simply stay where they are.
        uint64_t totalBytes = 0;
        std::string walkError;
        auto files = ingest::listFiles(*seedFrom, &totalBytes, &walkError);
        if (!walkError.empty()) {
            std::fprintf(stderr, "error: %s\n", walkError.c_str());
            return 1;
        }
        std::fprintf(stderr, "seeding from %s: %zu files, %.1f GB to hash (%s)\n", seedFrom->c_str(), files.size(),
                     double(totalBytes) / 1e9, seedMode == store::Store::Adopt::Move ? "move" : "clone");

        std::set<store::CKey> claimed;  // one adoption per CKey; later duplicates stay put
        std::mutex claimedMutex;
        auto want = [&](const store::CKey& key) {
            if (!wanted.count(key)) return false;
            std::lock_guard<std::mutex> lock(claimedMutex);
            return claimed.insert(key).second;
        };
        std::atomic<size_t> adopted{0};
        ingest::parallelFor(files.size(), "seeding", [&](size_t i) {
            std::string e;
            if (st.adoptFile(files[i], seedMode, want, &e)) adopted++;
        });
        std::fprintf(stderr, "adopted %zu objects from %zu seed files; the rest (stale, duplicate, not in this "
                     "build) were left untouched\n", adopted.load(), files.size());
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
        ULONGLONG size = 0;
        size_t clean = 0;
        DWORD code = ERROR_SUCCESS;
        bool ok = CascOpenFile(hStorage.get(), ckey.data(), 0, CASC_OPEN_BY_CKEY, hFile.out()) &&
                  CascGetFileSize64(hFile.get(), &size);
        if (ok) code = readAll(hFile.get(), size, data, &clean);
        else code = GetCascError();
        if (code != ERROR_SUCCESS) {
            ULONGLONG keyName = 0;
            char key[24] = "";
            if (code == ERROR_FILE_ENCRYPTED && CascGetNotFoundEncryptionKey(hStorage.get(), &keyName))
                std::snprintf(key, sizeof(key), "%016llX", static_cast<unsigned long long>(keyName));
            failures[ckey] = code == ERROR_FILE_ENCRYPTED ? std::string("encrypted:") + key
                                                          : "unreadable:" + std::to_string(code);
            // Mostly readable with a small encrypted part: keep the zero-filled
            // file too, as its own object (named by the md5 of what's stored, so
            // invariant 1 holds), and record it in partial.tsv. Consumers that
            // parse per section (husk's DB2 reader) use what's there.
            double unread = size ? 1.0 - double(clean) / double(size) : 1.0;
            if (code == ERROR_FILE_ENCRYPTED && unread <= storage::kMaxOvercomeEncryptedFraction &&
                CascSetFileFlags(hFile.get(), CASC_OVERCOME_ENCRYPTED) &&
                CascSetFilePointer64(hFile.get(), 0, nullptr, FILE_BEGIN) &&
                readAll(hFile.get(), size, data, &clean) == ERROR_SUCCESS) {
                store::CKey object = store::md5Bytes(data.data(), data.size());
                if (st.hasObject(object) || st.putBytes(object, data.data(), data.size(), &e)) {
                    partials[ckey] = {object, key, unread};
                    failures[ckey] = std::string("partial:") + key;
                }
            }
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
    store::writeTextAtomic(buildDir / "missing.tsv", missingTsv);

    // partial.tsv: zero-filled stand-ins for encrypted files (see the fetch
    // loop). Rows from earlier runs survive while their real object is still
    // missing, so a --mask or interrupted run never forgets one.
    {
        std::map<uint32_t, std::string> rowsByFdid;
        std::ifstream prev(buildDir / "partial.tsv");
        std::string line;
        std::getline(prev, line);  // header
        while (std::getline(prev, line)) {
            auto cells = std::vector<std::string>{};
            std::stringstream ss(line);
            for (std::string c; std::getline(ss, c, '\t');) cells.push_back(c);
            if (cells.size() < 3 || cells[1].size() != 32 || cells[2].size() != 32) continue;
            store::CKey real, object;
            for (size_t i = 0; i < 16; i++) {
                real[i] = uint8_t(std::stoul(cells[1].substr(2 * i, 2), nullptr, 16));
                object[i] = uint8_t(std::stoul(cells[2].substr(2 * i, 2), nullptr, 16));
            }
            if (!st.hasObject(real) && st.hasObject(object)) rowsByFdid[uint32_t(std::stoul(cells[0]))] = line;
        }
        for (const auto& [fdid, ckey] : ordered) {
            auto p = partials.find(ckey);
            if (p == partials.end()) continue;
            char frac[16];
            std::snprintf(frac, sizeof(frac), "%.4f", p->second.unreadAtLeast);
            rowsByFdid[fdid] = std::to_string(fdid) + "\t" + rootmanifest::hex(ckey) + "\t" +
                               rootmanifest::hex(p->second.object) + "\t" + p->second.keyName + "\t" + frac;
        }
        std::string tsv = "fdid\tckey\tobject\tkey\tunread_at_least\n";
        for (const auto& [fdid, row] : rowsByFdid) tsv += row + "\n";
        store::writeTextAtomic(buildDir / "partial.tsv", tsv);
        std::fprintf(stderr, "%zu FDIDs served zero-filled (partial.tsv)\n", rowsByFdid.size());
    }

    std::string meta = "{\n  \"label\": \"" + label + "\",\n  \"product\": \"" + product +
                       "\",\n  \"locale\": \"enUS\",\n  \"fdids\": " + std::to_string(selected.size()) +
                       ",\n  \"missing\": " + std::to_string(missingCount) + ",\n  \"snapshot_at\": \"" + nowIso() +
                       "\"\n}\n";
    store::writeTextAtomic(buildDir / "meta.json", meta);

    if (!tag.empty()) st.setTag(tag, label);
    std::fprintf(stderr, "build %s: extracted %zu, %zu FDIDs missing (see missing.tsv)%s\n", label.c_str(), fetched,
                 missingCount, tag.empty() ? "" : (", tagged " + tag).c_str());
    return 0;
}

}  // namespace commands
