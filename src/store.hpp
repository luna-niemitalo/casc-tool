#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rootmanifest.hpp"

// The build history store (DESIGN.md "Store layout"). Owns every path
// convention under <store>/ so commands never spell one out themselves:
//
//   objects/ab/cd/<ckey>   decoded bytes; md5(bytes) == ckey, always (invariant 1)
//   builds/<label>/        ROOT, manifest.tsv, missing.tsv, meta.json
//   tags/<name>            one line: the label it points at (live, ptr, beta)
//   tmp/                   staging for object writes; safe to empty when idle
namespace store {

using CKey = rootmanifest::CKey;

// md5 of a file's contents; nullopt (with *error) if it can't be read.
std::optional<CKey> md5File(const std::filesystem::path& path, std::string* error);
CKey md5Bytes(const uint8_t* data, size_t size);

// Writes via a .tmp sibling + rename, so a mount or a reader never sees a
// half-written manifest/meta file. False if the write failed.
bool writeTextAtomic(const std::filesystem::path& path, const std::string& text);

// Label order: digit runs compare as numbers, so 12.1.10.x sorts after
// 12.1.5.x and a product's builds line up oldest to newest.
bool labelLess(const std::string& a, const std::string& b);

class Store {
public:
    explicit Store(std::filesystem::path root) : root_(std::move(root)) {}

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path objectPath(const CKey& key) const;
    bool hasObject(const CKey& key) const;

    // Writes to tmp/, hashes what was actually written, and renames into
    // objects/ only on an exact CKey match -- a wrong or partial file can
    // never appear under a CKey name.
    bool putBytes(const CKey& key, const uint8_t* data, size_t size, std::string* error);

    enum class Adopt {
        Clone,  // copy_file_range into tmp/ (a ZFS >= 2.2 block clone on the same pool), re-hash, commit
        Move,   // rename the file itself into objects/: converts an export in place, no copy
    };
    // Adds an existing file under whatever CKey its content has, if `want`
    // says that CKey is needed: hashes `source` once, asks `want`, then
    // clones or moves it. Returns the CKey when adopted. A file that isn't
    // wanted (stale, duplicate, unknown) is left exactly where it was.
    // Move requires `source` on the store's filesystem with nothing else
    // writing it (hash and rename are separate steps).
    template <typename Want>
    std::optional<CKey> adoptFile(const std::filesystem::path& source, Adopt mode, Want&& want, std::string* error) {
        auto key = md5File(source, error);
        if (!key || !want(*key)) return std::nullopt;
        bool ok = mode == Adopt::Move ? moveIn(*key, source, error) : cloneIn(*key, source, error);
        return ok ? key : std::nullopt;
    }

    std::filesystem::path buildDir(const std::string& label) const { return root_ / "builds" / label; }

    // Records builds/<label>/ROOT plus the derived manifest.tsv. The same
    // bytes again is a resume (no-op); different bytes under an existing
    // label is refused -- a label always means exactly one ROOT.
    bool recordRoot(const std::string& label, const std::vector<uint8_t>& root,
                    const std::vector<rootmanifest::Row>& rows, std::string* error) const;
    std::vector<std::string> builds() const;  // in labelLess order

    // A build as typed on the command line: a label, or a tag naming one.
    // Labels win, so a tag can never shadow a stored build.
    std::optional<std::string> resolve(const std::string& name) const;

    // builds/<label>/meta.json as key -> value, string values unquoted. Reads
    // only the flat one-key-per-line objects snapshot and import write; empty
    // if there is none. Records from before 2026-11-09 may lack "product".
    std::map<std::string, std::string> meta(const std::string& label) const;

    // builds/<label>/ROOT, parsed: the build's authoritative record
    // (invariant 3). Empty with *error set for a path build or a bad ROOT.
    std::vector<rootmanifest::Row> readRoot(const std::string& label, std::string* error) const;

    std::map<std::string, std::string> tags() const;
    void setTag(const std::string& name, const std::string& label) const;

private:
    std::filesystem::path stagingPath() const;
    bool commit(const CKey& key, const std::filesystem::path& staged, std::string* error);
    bool cloneIn(const CKey& key, const std::filesystem::path& source, std::string* error);
    bool moveIn(const CKey& key, const std::filesystem::path& source, std::string* error);

    std::filesystem::path root_;
};

}  // namespace store
