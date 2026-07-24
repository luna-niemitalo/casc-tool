#include <cstdio>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "storage.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() { return storage::commonOptionSpecs(); }

const char* kUsage = "casc-tool extract <id-or-path> [out-file] [options]";

// Picks a sane default output filename when the caller doesn't give one:
// the path's own basename if a path was given, or CascLib's own
// FILE########.dat convention if only a FileDataID was given (matches what
// CascLib itself falls back to for entries with no known name).
std::string defaultOutFile(const std::string& idOrPath) {
    if (storage::looksLikeFileDataId(idOrPath)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), CASC_FILEID_FORMAT, static_cast<unsigned>(std::stoul(idOrPath)));
        return buf;
    }
    std::string p = idOrPath;
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

}  // namespace

void helpExtract() {
    std::printf(
        "casc-tool extract -- extract a single file to disk\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <id-or-path>   a FileDataID (e.g. 1234) or an in-storage path\n"
        "  [out-file]     where to write it. Default: the file's own basename\n"
        "                 (or FILE########.dat if given a bare ID) in the\n"
        "                 current directory\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool extract character/bloodelf/female/bloodelffemale.m2\n"
        "  casc-tool extract 1234 out/logo.avi\n");
}

int runExtract(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(1, 2, kUsage);
    std::string idOrPath = args.positionals()[0];
    std::string outFile =
        args.positionals().size() > 1 ? args.positionals()[1] : defaultOutFile(idOrPath);

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    storage::FileHandle hFile;
    std::string openError;
    if (!storage::openFile(hStorage.get(), idOrPath, storage::listFileFromArgs(args), hFile, &openError)) {
        std::fprintf(stderr, "error: %s\n", openError.c_str());
        return 1;
    }

    uint64_t bytesWritten = 0;
    std::string error;
    if (!storage::copyToFile(hFile.get(), outFile, &bytesWritten, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::fprintf(stderr, "extracted %s -> %s (%llu bytes)\n", idOrPath.c_str(), outFile.c_str(),
                 static_cast<unsigned long long>(bytesWritten));
    return 0;
}

}  // namespace commands
