#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Shared by snapshot's --seed-from and import: walking a tree of loose files
// and hashing them in parallel. One place for the walk rules (regular files
// only, symlinks skipped) and the worker count.
namespace ingest {

// Every regular, non-symlink file under `dir`, plus their total size.
// Empty with *error set if the walk itself fails.
std::vector<std::filesystem::path> listFiles(const std::filesystem::path& dir, uint64_t* totalBytes,
                                             std::string* error);

// True if `tree` is `store` or one of its ancestors. A source tree *inside*
// the store (e.g. <store>/legacy) is fine; one containing it would walk objects/.
bool containsStore(const std::filesystem::path& tree, const std::filesystem::path& store);

// Calls fn(i) for every i in [0, count) on a fixed pool of workers, printing
// "<phase>: done/count" to stderr. fn must be safe to call concurrently.
void parallelFor(size_t count, const char* phase, const std::function<void(size_t)>& fn);

}  // namespace ingest
