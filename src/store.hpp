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

class Store {
public:
    explicit Store(std::filesystem::path root) : root_(std::move(root)) {}

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path objectPath(const CKey& key) const;
    bool hasObject(const CKey& key) const;

    // Both write to tmp/, hash what was actually written, and rename into
    // objects/ only on an exact CKey match -- a wrong or partial file can
    // never appear under a CKey name. putFile copies with copy_file_range,
    // which ZFS (>= 2.2, block cloning) turns into a clone, not a copy.
    bool putBytes(const CKey& key, const uint8_t* data, size_t size, std::string* error);
    bool putFile(const CKey& key, const std::filesystem::path& source, std::string* error);

    std::filesystem::path buildDir(const std::string& label) const { return root_ / "builds" / label; }
    std::vector<std::string> builds() const;

    std::map<std::string, std::string> tags() const;
    void setTag(const std::string& name, const std::string& label) const;

private:
    std::filesystem::path stagingPath() const;
    bool commit(const CKey& key, const std::filesystem::path& staged, std::string* error);

    std::filesystem::path root_;
};

// md5 of a file's contents; nullopt (with *error) if it can't be read.
std::optional<CKey> md5File(const std::filesystem::path& path, std::string* error);

}  // namespace store
