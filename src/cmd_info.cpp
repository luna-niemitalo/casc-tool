#include <cstdio>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "storage.hpp"

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--format", true, "<text|json>", "Output format. Default: text"});
    return s;
}

const char* kUsage = "casc-tool info <id-or-path> [options]";

std::string hex(const BYTE* bytes, size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; i++) {
        out += digits[(bytes[i] >> 4) & 0xF];
        out += digits[bytes[i] & 0xF];
    }
    return out;
}

}  // namespace

void helpInfo() {
    std::printf(
        "casc-tool info -- show metadata for one file, without extracting it\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <id-or-path>   a FileDataID (e.g. 1234) or an in-storage path\n"
        "                 (e.g. character/bloodelf/female/bloodelffemale.m2)\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool info 1234\n"
        "  casc-tool info character/bloodelf/female/bloodelffemale.m2\n");
}

int runInfo(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(1, 1, kUsage);
    std::string idOrPath = args.positionals()[0];
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text or json (got '%s')\n", fmt.c_str());
        return 2;
    }

    storage::StorageHandle hStorage;
    if (!storage::open(storage::fromArgs(args), hStorage)) return 1;

    storage::FileHandle hFile;
    std::string openError;
    if (!storage::openFile(hStorage.get(), idOrPath, storage::listFileFromArgs(args), hFile, &openError)) {
        std::fprintf(stderr, "error: %s\n", openError.c_str());
        return 1;
    }

    CASC_FILE_FULL_INFO info = {};
    size_t needed = 0;
    if (!CascGetFileInfo(hFile.get(), CascFileFullInfo, &info, sizeof(info), &needed)) {
        std::fprintf(stderr, "error: couldn't read file info for '%s': %s\n", idOrPath.c_str(),
                     storage::errorMessage(GetCascError()).c_str());
        return 1;
    }

    std::string ckey = hex(info.CKey, MD5_HASH_SIZE);
    std::string ekey = hex(info.EKey, MD5_HASH_SIZE);

    if (fmt == "json") {
        std::printf(
            "{\"fdid\":%u,\"ckey\":\"%s\",\"ekey\":\"%s\",\"data_file\":\"%s\","
            "\"content_size\":%llu,\"encoded_size\":%llu,\"span_count\":%u,"
            "\"locale_flags\":%u,\"content_flags\":%u}\n",
            info.FileDataId, ckey.c_str(), ekey.c_str(), info.DataFileName,
            static_cast<unsigned long long>(info.ContentSize),
            static_cast<unsigned long long>(info.EncodedSize), info.SpanCount, info.LocaleFlags,
            info.ContentFlags);
    } else {
        std::printf("FileDataID:    %u\n", info.FileDataId);
        std::printf("CKey:          %s\n", ckey.c_str());
        std::printf("EKey:          %s\n", ekey.c_str());
        std::printf("data file:     %s\n", info.DataFileName);
        std::printf("content size:  %llu bytes\n", static_cast<unsigned long long>(info.ContentSize));
        std::printf("encoded size:  %llu bytes\n", static_cast<unsigned long long>(info.EncodedSize));
        std::printf("span count:    %u\n", info.SpanCount);
        std::printf("locale flags:  0x%x\n", info.LocaleFlags);
        std::printf("content flags: 0x%x%s\n", info.ContentFlags,
                     (info.ContentFlags & CASC_CFLAG_ENCRYPTED) ? "  (encrypted -- see --keys)" : "");
    }
    return 0;
}

}  // namespace commands
