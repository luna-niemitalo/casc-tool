#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"

// Shared plumbing for every command that opens a CASC storage: RAII handle
// wrappers (so a thrown ArgError or an early return can never leak a CascLib
// handle), a friendly error-message translator, locale-name parsing, and the
// actual open-with-options call.
namespace storage {

// Closes a CascLib HANDLE automatically. CascLib has three handle kinds
// (storage/file/find) with three different close functions; this is
// templated on which one to call so callers never have to remember.
template <bool (*CloseFn)(HANDLE)>
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    void reset(HANDLE h = nullptr) {
        if (h_ != nullptr) CloseFn(h_);
        h_ = h;
    }

    HANDLE* out() { return &h_; }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

using StorageHandle = Handle<CascCloseStorage>;
using FileHandle = Handle<CascCloseFile>;
using FindHandle = Handle<CascFindClose>;

// Options shared by every command that opens a storage. Each command's own
// OptionSpec list includes these (see commands.hpp) so --help stays
// consistent across list/info/extract/extract-batch.
struct OpenOptions {
    std::string path;                  // storage root, e.g. "external_data"
    std::string locale = "all";        // human name, see parseLocale()
    std::optional<std::string> keysFile;  // TACT-keys-format file, see README
    std::optional<std::string> product;   // codename, for multi-product installs
};

// The --storage/--listfile/--locale/--keys/--product options every
// storage-opening command shares, so their --help output (and behavior)
// stays identical instead of subtly drifting between commands.
std::vector<cli::OptionSpec> commonOptionSpecs();

// Pulls OpenOptions out of already-parsed args using the specs above.
OpenOptions fromArgs(const cli::Args& args);

// The --listfile value out of already-parsed args (kept separate from
// OpenOptions since it's consumed by CascFindFirstFile, not CascOpenStorage).
std::string listFileFromArgs(const cli::Args& args);

// Turns a human-friendly locale name ("all", "enus", "dede", ...) into a
// CASC_LOCALE_* mask. Throws cli::ArgError (via std::runtime_error) with the
// full list of valid names if given something unrecognized.
DWORD parseLocale(const std::string& name);

// Opens the storage, applies --keys if given, prints a one-line banner to
// stderr ("opened <product> build <N>, <count> files") so commands stay
// silent on stdout except for their actual data output. Returns false (with
// a message already printed to stderr) on failure.
bool open(const OpenOptions& opts, StorageHandle& out);

// Translates a GetCascError() code into a human-readable message. On Linux,
// CascLib's low-numbered codes are literal errno values (see CascPort.h),
// so this is strerror() for those plus a lookup table for CascLib's own
// codes (>= 1000).
std::string errorMessage(uint32_t cascError);

// True if the whole string is decimal digits -- used to tell a FileDataID
// apart from a path on the command line ("info 1234" vs "info some/path.m2").
bool looksLikeFileDataId(const std::string& s);

// Opens a file given either a FileDataID (all-digit string) or an in-storage
// path. Note this always resolves through a FileDataID lookup, even for
// paths: WoW's CASC root stores name *hashes*, not names, so CascOpenFile's
// by-name mode only succeeds for names CascLib has already learned via a
// Find pass on this same handle (confirmed empirically -- opening a file
// that `list` had just resolved by name, without going through Find first,
// failed with ERROR_FILE_NOT_FOUND). Resolving the path to a FileDataID via
// CascFindFirstFile(exact path, listFile) first and then opening by ID
// sidesteps that entirely and is what every other command already does.
bool openFile(HANDLE hStorage, const std::string& idOrPath, const std::string& listFile, FileHandle& out);

// Streams an already-open file to disk in chunks, creating parent
// directories as needed. Shared by `extract` and `extract-batch` so the
// read/write loop and its error handling exist in exactly one place.
// On failure, returns false and *errorOut is set to a human-readable reason
// (either from GetCascError() or a plain errno message for the local
// filesystem side).
bool copyToFile(HANDLE hFile, const std::string& outPath, uint64_t* bytesWritten, std::string* errorOut);

}  // namespace storage
