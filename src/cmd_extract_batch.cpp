#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "listfile.hpp"
#include "storage.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--from-list", true, "<file>",
                 "Extract exactly these FileDataIDs (one decimal ID per line) instead of "
                 "matching a mask -- one storage open, no glob walk. Replaces <mask>; still "
                 "takes <out-dir>"});
    s.push_back({"--unresolved-only", false, "",
                 "Only extract entries with no name in --listfile (written as "
                 "<out-dir>/_unresolved/FILE########.dat). Not valid with --from-list -- "
                 "an explicit ID list is neither resolved nor unresolved, it's just itself"});
    s.push_back({"--dry-run", false, "", "Report what would be extracted (count, total bytes) without writing anything"});
    s.push_back({"--strict-encrypted", false, "",
                 "Never write a file that's partially encrypted with a missing key, even if only a small "
                 "fraction of it is affected -- the default instead zero-fills and proceeds under 30%"});
    return s;
}

const char* kUsage = "casc-tool extract-batch <mask> <out-dir> [options]";
const char* kUsageFromList = "casc-tool extract-batch --from-list <ids-file> <out-dir> [options]";

// The FILE########.dat fallback shared by both output-path builders below:
// used both for a genuinely nameless entry and for a name that sanitized
// away to nothing (e.g. a listfile entry that was only ".." components) --
// in both cases there's no usable real name, so both land the same place.
std::string unresolvedPath(unsigned fileDataId, const std::string& outDir) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "_unresolved/" CASC_FILEID_FORMAT, fileDataId);
    return outDir + "/" + buf;
}

std::string outPathFor(const CASC_FIND_DATA& fd, const std::string& outDir) {
    if (fd.NameType == CascNameFull) {
        std::string rel = storage::sanitizeRelativePath(fd.szFileName);
        if (!rel.empty()) return outDir + "/" + rel;
    }
    return unresolvedPath(fd.dwFileDataId, outDir);
}

// Same naming convention as outPathFor above, but for the --from-list path:
// there's no CASC_FIND_DATA (a bare-ID open skips the Find/glob-walk step
// entirely), so the real name -- if any -- comes from the already-loaded
// --listfile map instead of CascLib's own NameType.
std::string outPathForId(unsigned id, const std::map<unsigned, std::string>& names, const std::string& outDir) {
    auto it = names.find(id);
    if (it != names.end()) {
        std::string rel = storage::sanitizeRelativePath(it->second);
        if (!rel.empty()) return outDir + "/" + rel;
    }
    return unresolvedPath(id, outDir);
}

void reportProgress(std::chrono::steady_clock::time_point& lastReport, unsigned long long extracted,
                     unsigned long long failed, uint64_t totalBytes) {
    auto now = std::chrono::steady_clock::now();
    if (now - lastReport <= std::chrono::milliseconds(200)) return;
    std::fprintf(stderr, "\rextracted %llu, failed %llu, %llu bytes...\033[K", extracted, failed,
                 static_cast<unsigned long long>(totalBytes));
    std::fflush(stderr);
    lastReport = now;
}

// The --from-list path: one storage open, then one CascOpenFile-by-ID per
// entry in the file -- no CascFindFirstFile/glob walk at all, which is the
// whole point (see the flag's own --help text: a Find-based mask walk opens
// and re-scans the *entire* root for every invocation, which is fine for a
// handful of masks but not for a worklist of thousands of explicit IDs).
int runExtractFromList(const cli::Args& args, const std::string& idsPath, const std::string& outDir) {
    std::string listFile = storage::listFileFromArgs(args);
    bool dryRun = args.flag("--dry-run");

    // --from-list itself is checked first, ahead of --listfile: it's the
    // flag the caller of this mode just typed, so if *it's* the one that's
    // wrong, that's the more relevant error to see first -- rather than a
    // report about the unrelated (possibly still-default) --listfile value,
    // which the caller may not have been thinking about at all.
    std::string idListError;
    auto ids = listfile::loadIdList(idsPath, &idListError);
    if (!idListError.empty()) {
        std::fprintf(stderr, "error: couldn't read --from-list file: %s\n", idListError.c_str());
        return 1;
    }
    if (ids.empty()) {
        std::fprintf(stderr, "error: --from-list '%s' contained no valid FileDataIDs (expected one decimal ID per line)\n",
                     idsPath.c_str());
        return 1;
    }

    std::string listFileError;
    if (!storage::checkListFileExists(listFile, &listFileError)) {
        std::fprintf(stderr, "error: %s\n", listFileError.c_str());
        return 1;
    }
    std::string mapError;
    auto names = listfile::load(listFile, &mapError);

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    bool allowOvercomeEncrypted = !args.flag("--strict-encrypted");
    unsigned long long extracted = 0, failed = 0, notInStorage = 0, notAvailable = 0, partiallyEncrypted = 0;
    uint64_t totalBytes = 0, plannedBytes = 0;
    auto lastReport = std::chrono::steady_clock::now();

    for (unsigned id : ids) {
        storage::FileHandle hFile;
        if (!CascOpenFile(hStorage.get(), CASC_FILE_DATA_ID(id), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID, hFile.out())) {
            failed++;
            notInStorage++;
            std::fprintf(stderr, "\nwarning: FileDataID %u: no such file in this storage\n", id);
            reportProgress(lastReport, extracted, failed, totalBytes);
            continue;
        }

        // A bare-ID open skips CascFindFirstFile's bFileAvailable check
        // entirely (see storage::openFile's own comment on this), so even an
        // ID CascOpenFile accepted can still point at data this local
        // install never downloaded -- confirm via CascGetFileInfo before
        // counting it as real, same as storage::openFile does.
        CASC_FILE_FULL_INFO info{};
        size_t needed = 0;
        if (!CascGetFileInfo(hFile.get(), CascFileFullInfo, &info, sizeof(info), &needed)) {
            failed++;
            notAvailable++;
            std::fprintf(stderr,
                         "\nwarning: FileDataID %u: known but not available in this local install "
                         "(likely optional/legacy content that was never downloaded)\n",
                         id);
            reportProgress(lastReport, extracted, failed, totalBytes);
            continue;
        }
        plannedBytes += info.ContentSize;

        if (dryRun) continue;

        std::string outPath = outPathForId(id, names, outDir);
        uint64_t written = 0;
        std::string error, overcomeNote;
        if (storage::copyToFile(hFile.get(), outPath, &written, &error, allowOvercomeEncrypted, &overcomeNote)) {
            extracted++;
            totalBytes += written;
            if (!overcomeNote.empty()) {
                partiallyEncrypted++;
                std::fprintf(stderr, "\nwarning: %s (fdid=%u): %s\n", outPath.c_str(), id, overcomeNote.c_str());
            }
        } else {
            failed++;
            std::fprintf(stderr, "\nwarning: %s (fdid=%u): %s\n", outPath.c_str(), id, error.c_str());
        }
        reportProgress(lastReport, extracted, failed, totalBytes);
    }

    std::fprintf(stderr, "\r\033[K");
    if (dryRun) {
        std::printf("would extract %llu/%llu files, %llu bytes\n",
                    static_cast<unsigned long long>(ids.size()) - notInStorage - notAvailable,
                    static_cast<unsigned long long>(ids.size()), static_cast<unsigned long long>(plannedBytes));
    } else {
        std::printf("extracted %llu/%llu files (%llu failed), %llu bytes -> %s\n", extracted,
                    static_cast<unsigned long long>(ids.size()), failed, static_cast<unsigned long long>(totalBytes),
                    outDir.c_str());
    }
    if (notInStorage > 0 || notAvailable > 0) {
        std::fprintf(stderr, "%llu not in this storage at all, %llu known but not locally available\n", notInStorage,
                     notAvailable);
    }
    if (partiallyEncrypted > 0) {
        std::fprintf(stderr, "%llu file(s) partially zero-filled (encrypted content, key missing -- see --keys)\n",
                     partiallyEncrypted);
    }
    return failed > 0 ? 1 : 0;
}

}  // namespace

std::vector<cli::OptionSpec> extractBatchSpecs() { return specs(); }

void helpExtractBatch() {
    std::printf(
        "casc-tool extract-batch -- bulk-extract matching files into a directory\n"
        "\n"
        "usage: %s\n"
        "       %s\n"
        "\n"
        "  <mask>       glob to match in-storage paths, e.g. 'character/bloodelf/*'\n"
        "  --from-list  a file of explicit FileDataIDs (one per line) instead of a\n"
        "               mask -- use this over a mask when you already know exactly\n"
        "               which IDs you want (e.g. another tool's worklist); one\n"
        "               storage open total instead of one glob walk\n"
        "  <out-dir>    destination directory (created if missing). Files land at\n"
        "               <out-dir>/<in-game path>, mirroring the game's own layout;\n"
        "               entries with no known name go under <out-dir>/_unresolved/\n"
        "\n"
        "options:\n",
        kUsage, kUsageFromList);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool extract-batch --dry-run 'character/bloodelf/*' out/\n"
        "                                    # check size/count first\n"
        "  casc-tool extract-batch 'character/bloodelf/*' out/\n"
        "  casc-tool extract-batch --from-list missing_ids.txt out/\n");
}

int runExtractBatch(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    auto fromList = args.option("--from-list");

    if (fromList) {
        if (args.flag("--unresolved-only")) {
            std::fprintf(stderr,
                         "error: --unresolved-only isn't valid with --from-list -- an explicit ID list is "
                         "neither resolved nor unresolved, it's just itself\n");
            return 2;
        }
        args.requirePositionals(1, 1, kUsageFromList);
        return runExtractFromList(args, *fromList, args.positionals()[0]);
    }

    args.requirePositionals(2, 2, kUsage);
    std::string mask = args.positionals()[0];
    std::string outDir = args.positionals()[1];
    std::string listFile = storage::listFileFromArgs(args);
    bool unresolvedOnly = args.flag("--unresolved-only");
    bool dryRun = args.flag("--dry-run");

    std::string listFileError;
    if (!storage::checkListFileExists(listFile, &listFileError)) {
        std::fprintf(stderr, "error: %s\n", listFileError.c_str());
        return 1;
    }

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    CASC_FIND_DATA fd{};
    storage::FindHandle hFind(CascFindFirstFile(hStorage.get(), mask.c_str(), &fd, listFile.c_str()));
    // See storage::openFile's comment: a literal (non-wildcard) mask
    // matching nothing has been observed to still return a non-null handle
    // with fd left holding CASC_INVALID_ID/garbage.
    if (!hFind || fd.dwFileDataId == CASC_INVALID_ID) {
        std::fprintf(stderr, "error: no files matched '%s'\n", mask.c_str());
        return 1;
    }

    bool allowOvercomeEncrypted = !args.flag("--strict-encrypted");
    unsigned long long matched = 0, extracted = 0, failed = 0, skippedNoId = 0, partiallyEncrypted = 0;
    uint64_t totalBytes = 0, plannedBytes = 0;
    auto lastReport = std::chrono::steady_clock::now();

    do {
        bool resolved = fd.NameType == CascNameFull;
        if (unresolvedOnly && resolved) continue;

        // Entries with no FileDataID at all (CKey/EKey-only CASC
        // components, not game assets -- see CHANGELOG.md #2/#3) can never
        // be opened via CASC_FILE_DATA_ID; CascOpenFile always fails for
        // them. A broad mask like '*' can match well over a million of
        // these on a real install, so skip them outright instead of
        // attempting the open and logging a per-file warning for each one
        // -- that would drown out any genuinely actionable failure in the
        // same run.
        if (fd.dwFileDataId == CASC_INVALID_ID) {
            skippedNoId++;
            continue;
        }

        matched++;
        plannedBytes += fd.FileSize;

        if (dryRun) continue;

        std::string outPath = outPathFor(fd, outDir);
        storage::FileHandle hFile;
        bool ok = CascOpenFile(hStorage.get(), CASC_FILE_DATA_ID(fd.dwFileDataId), CASC_LOCALE_ALL,
                                CASC_OPEN_BY_FILEID, hFile.out());
        std::string error, overcomeNote;
        uint64_t written = 0;
        if (ok) ok = storage::copyToFile(hFile.get(), outPath, &written, &error, allowOvercomeEncrypted, &overcomeNote);

        if (ok) {
            extracted++;
            totalBytes += written;
            if (!overcomeNote.empty()) {
                partiallyEncrypted++;
                std::fprintf(stderr, "\nwarning: %s (fdid=%u): %s\n", outPath.c_str(), fd.dwFileDataId,
                             overcomeNote.c_str());
            }
        } else {
            failed++;
            std::fprintf(stderr, "\nwarning: %s (fdid=%u): %s\n", outPath.c_str(), fd.dwFileDataId,
                         error.empty() ? storage::errorMessage(GetCascError()).c_str() : error.c_str());
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::milliseconds(200)) {
            std::fprintf(stderr, "\rextracted %llu, failed %llu, %llu bytes...\033[K", extracted, failed,
                         static_cast<unsigned long long>(totalBytes));
            std::fflush(stderr);
            lastReport = now;
        }
    } while (CascFindNextFile(hFind.get(), &fd));

    std::fprintf(stderr, "\r\033[K");
    if (dryRun) {
        std::printf("would extract %llu files, %llu bytes\n", matched, static_cast<unsigned long long>(plannedBytes));
    } else {
        std::printf("extracted %llu/%llu files (%llu failed), %llu bytes -> %s\n", extracted, matched, failed,
                    static_cast<unsigned long long>(totalBytes), outDir.c_str());
    }
    if (skippedNoId > 0) {
        std::fprintf(stderr, "skipped %llu entries with no FileDataID (not extractable game assets)\n", skippedNoId);
    }
    if (partiallyEncrypted > 0) {
        std::fprintf(stderr, "%llu file(s) partially zero-filled (encrypted content, key missing -- see --keys)\n",
                     partiallyEncrypted);
    }
    return failed > 0 ? 1 : 0;
}

}  // namespace commands
