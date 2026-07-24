#include "storage.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <vector>

namespace storage {

namespace {

const std::map<std::string, DWORD>& localeTable() {
    static const std::map<std::string, DWORD> table = {
        {"all", CASC_LOCALE_ALL},   {"none", CASC_LOCALE_NONE}, {"enus", CASC_LOCALE_ENUS},
        {"kokr", CASC_LOCALE_KOKR}, {"frfr", CASC_LOCALE_FRFR}, {"dede", CASC_LOCALE_DEDE},
        {"zhcn", CASC_LOCALE_ZHCN}, {"eses", CASC_LOCALE_ESES}, {"zhtw", CASC_LOCALE_ZHTW},
        {"engb", CASC_LOCALE_ENGB}, {"encn", CASC_LOCALE_ENCN}, {"entw", CASC_LOCALE_ENTW},
        {"esmx", CASC_LOCALE_ESMX}, {"ruru", CASC_LOCALE_RURU}, {"ptbr", CASC_LOCALE_PTBR},
        {"itit", CASC_LOCALE_ITIT}, {"ptpt", CASC_LOCALE_PTPT},
    };
    return table;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool WINAPI progressCallback(void*, CASC_PROGRESS_MSG msg, LPCSTR object, DWORD current, DWORD total) {
    const char* what = "Working";
    switch (msg) {
        case CascProgressLoadingFile: what = "Loading file"; break;
        case CascProgressLoadingManifest: what = "Loading manifest"; break;
        case CascProgressDownloadingFile: what = "Downloading file"; break;
        case CascProgressLoadingIndexes: what = "Loading indexes"; break;
        case CascProgressDownloadingArchiveIndexes: what = "Downloading archive indexes"; break;
    }
    if (total > 0) {
        std::fprintf(stderr, "\r%s: %s (%u/%u)\033[K", what, object ? object : "", current, total);
    } else {
        std::fprintf(stderr, "\r%s: %s\033[K", what, object ? object : "");
    }
    std::fflush(stderr);
    return false;  // never cancel
}

}  // namespace

DWORD parseLocale(const std::string& name) {
    auto it = localeTable().find(toLower(name));
    if (it != localeTable().end()) return it->second;

    std::string valid;
    for (const auto& [key, value] : localeTable()) {
        (void)value;
        if (!valid.empty()) valid += ", ";
        valid += key;
    }
    throw std::runtime_error("unknown --locale '" + name + "', valid values: " + valid);
}

bool open(const OpenOptions& opts, StorageHandle& out) {
    CASC_OPEN_STORAGE_ARGS args = {};
    args.Size = sizeof(args);
    args.szLocalPath = opts.path.c_str();
    args.dwLocaleMask = parseLocale(opts.locale);
    args.PfnProgressCallback = progressCallback;
    if (opts.product) args.szCodeName = opts.product->c_str();

    HANDLE h = nullptr;
    bool ok = CascOpenStorageEx(nullptr, &args, false, &h);
    std::fprintf(stderr, "\r\033[K");  // clear the last progress line, if any

    if (!ok) {
        std::fprintf(stderr, "error: couldn't open storage '%s': %s\n", opts.path.c_str(),
                     errorMessage(GetCascError()).c_str());
        std::fprintf(stderr, "       (expecting a directory containing .build.info -- see README.md §2.1)\n");
        return false;
    }
    out.reset(h);

    if (opts.keysFile) {
        if (!CascImportKeysFromFile(out.get(), opts.keysFile->c_str())) {
            std::fprintf(stderr, "warning: couldn't load --keys file '%s': %s\n", opts.keysFile->c_str(),
                         errorMessage(GetCascError()).c_str());
        }
    }

    CASC_STORAGE_PRODUCT product = {};
    DWORD totalFiles = 0;
    size_t needed = 0;
    CascGetStorageInfo(out.get(), CascStorageProduct, &product, sizeof(product), &needed);
    CascGetStorageInfo(out.get(), CascStorageTotalFileCount, &totalFiles, sizeof(totalFiles), &needed);
    std::fprintf(stderr, "opened storage: product=%s build=%u total_files=%u\n",
                 product.szCodeName[0] ? product.szCodeName : "?", product.BuildNumber, totalFiles);
    return true;
}

std::string errorMessage(uint32_t cascError) {
    switch (cascError) {
        case ERROR_BAD_FORMAT: return "bad/unrecognized file format";
        case ERROR_NO_MORE_FILES: return "no more files";
        case ERROR_HANDLE_EOF: return "end of file";
        case ERROR_CAN_NOT_COMPLETE: return "operation can't complete";
        case ERROR_FILE_CORRUPT: return "file data is corrupt";
        case ERROR_FILE_ENCRYPTED: return "file is encrypted and the decryption key is missing (see --keys)";
        case ERROR_FILE_TOO_LARGE: return "file too large, or a required file part is missing";
        case ERROR_ARITHMETIC_OVERFLOW: return "arithmetic overflow, or file not available locally (needs online/CDN mode)";
        case ERROR_NETWORK_NOT_AVAILABLE: return "network not available, or buffer overflow";
        case ERROR_CANCELLED: return "operation cancelled";
        case ERROR_INDEX_PARSING_DONE: return "index parsing done";
        case ERROR_REPARSE_ROOT: return "root file reparse";
        case ERROR_CKEY_ALREADY_OPENED: return "file with this content key was already opened once (see CASC_OPEN_CKEY_ONCE)";
        default: {
            const char* msg = std::strerror(static_cast<int>(cascError));
            return msg ? msg : ("unknown error " + std::to_string(cascError));
        }
    }
}

std::vector<cli::OptionSpec> commonOptionSpecs() {
    return {
        {"--storage", true, "<path>", "CASC storage root (a directory containing .build.info). Default: external_data"},
        {"--listfile", true, "<path>", "FileDataID<->path mapping, e.g. wow-listfile's community-listfile.csv. Default: m2mod/mappings/listfile.csv"},
        {"--locale", true, "<name>", "One of: all (default), none, enus, engb, dede, frfr, eses, esmx, ruru, ptbr, ptpt, itit, kokr, zhcn, zhtw, encn, entw"},
        {"--keys", true, "<path>", "Optional TACT decryption keys file (same format as wow.tools's tactkeys API: 'KeyName KeyValue' hex pairs, one per line)"},
        {"--product", true, "<codename>", "Optional product codename, for installs with more than one flavor (e.g. 'wow', 'wowt' for PTR)"},
    };
}

OpenOptions fromArgs(const cli::Args& args) {
    OpenOptions opts;
    opts.path = args.optionOr("--storage", "external_data");
    opts.locale = args.optionOr("--locale", "all");
    opts.keysFile = args.option("--keys");
    opts.product = args.option("--product");
    return opts;
}

std::string listFileFromArgs(const cli::Args& args) {
    return args.optionOr("--listfile", "m2mod/mappings/listfile.csv");
}

bool looksLikeFileDataId(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool openFile(HANDLE hStorage, const std::string& idOrPath, const std::string& listFile, FileHandle& out) {
    DWORD fileDataId;

    if (looksLikeFileDataId(idOrPath)) {
        fileDataId = static_cast<DWORD>(std::stoul(idOrPath));
    } else {
        CASC_FIND_DATA fd;
        HANDLE hFindRaw = CascFindFirstFile(hStorage, idOrPath.c_str(), &fd, listFile.c_str());
        if (!hFindRaw) return false;
        FindHandle hFind(hFindRaw);
        fileDataId = fd.dwFileDataId;
    }

    HANDLE h = nullptr;
    bool ok = CascOpenFile(hStorage, CASC_FILE_DATA_ID(fileDataId), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID, &h);
    if (ok) out.reset(h);
    return ok;
}

bool copyToFile(HANDLE hFile, const std::string& outPath, uint64_t* bytesWritten, std::string* errorOut) {
    std::filesystem::path path(outPath);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            *errorOut = "couldn't create directory '" + path.parent_path().string() + "': " + ec.message();
            return false;
        }
    }

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        *errorOut = "couldn't create '" + outPath + "': " + std::strerror(errno);
        return false;
    }

    constexpr DWORD kChunk = 1 << 20;  // 1 MiB
    std::vector<char> buffer(kChunk);
    uint64_t total = 0;
    bool ok = true;

    while (true) {
        DWORD bytesRead = 0;
        if (!CascReadFile(hFile, buffer.data(), kChunk, &bytesRead)) {
            *errorOut = "read failed after " + std::to_string(total) + " bytes: " + errorMessage(GetCascError());
            ok = false;
            break;
        }
        if (bytesRead == 0) break;
        std::fwrite(buffer.data(), 1, bytesRead, out);
        total += bytesRead;
    }
    std::fclose(out);
    if (bytesWritten) *bytesWritten = total;
    return ok;
}

}  // namespace storage
