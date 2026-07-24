#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "storage.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--unresolved-only", false, "",
                 "Only extract entries with no name in --listfile (written as "
                 "<out-dir>/_unresolved/FILE########.dat)"});
    s.push_back({"--dry-run", false, "", "Report what would be extracted (count, total bytes) without writing anything"});
    return s;
}

const char* kUsage = "casc-tool extract-batch <mask> <out-dir> [options]";

std::string outPathFor(const CASC_FIND_DATA& fd, const std::string& outDir) {
    if (fd.NameType == CascNameFull) {
        std::string rel = fd.szFileName;
        for (char& c : rel) {
            if (c == '\\') c = '/';
        }
        return outDir + "/" + rel;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "_unresolved/" CASC_FILEID_FORMAT, fd.dwFileDataId);
    return outDir + "/" + buf;
}

}  // namespace

void helpExtractBatch() {
    std::printf(
        "casc-tool extract-batch -- bulk-extract matching files into a directory\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <mask>     glob to match in-storage paths, e.g. 'character/bloodelf/*'\n"
        "  <out-dir>  destination directory (created if missing). Files land at\n"
        "             <out-dir>/<in-game path>, mirroring the game's own layout;\n"
        "             entries with no known name go under <out-dir>/_unresolved/\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool extract-batch --dry-run 'character/bloodelf/*' out/\n"
        "                                    # check size/count first\n"
        "  casc-tool extract-batch 'character/bloodelf/*' out/\n");
}

int runExtractBatch(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
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

    unsigned long long matched = 0, extracted = 0, failed = 0, skippedNoId = 0;
    uint64_t totalBytes = 0, plannedBytes = 0;
    auto lastReport = std::chrono::steady_clock::now();

    do {
        bool resolved = fd.NameType == CascNameFull;
        if (unresolvedOnly && resolved) continue;

        // Entries with no FileDataID at all (CKey/EKey-only CASC
        // components, not game assets -- see FAILURES.md #2/#3) can never
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
        std::string error;
        uint64_t written = 0;
        if (ok) ok = storage::copyToFile(hFile.get(), outPath, &written, &error);

        if (ok) {
            extracted++;
            totalBytes += written;
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
    return failed > 0 ? 1 : 0;
}

}  // namespace commands
