#include "ingest.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace fs = std::filesystem;

namespace ingest {

namespace {

// Hashing loose files is bound by one ZFS pool's reads, not CPU; 8 readers
// saturated it in practice without starving the desktop. Not a flag until a
// second machine shows a different sweet spot.
constexpr unsigned kThreads = 8;

void progress(const char* phase, size_t done, size_t total) {
    std::fprintf(stderr, "\r%s: %zu/%zu", phase, done, total);
    std::fflush(stderr);
}

}  // namespace

std::vector<fs::path> listFiles(const fs::path& dir, uint64_t* totalBytes, std::string* error) {
    std::vector<fs::path> files;
    *totalBytes = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it->is_symlink(ec) || !it->is_regular_file(ec)) continue;
        files.push_back(it->path());
        *totalBytes += it->file_size(ec);
    }
    if (ec) {
        *error = "walking " + dir.string() + ": " + ec.message();
        return {};
    }
    return files;
}

bool containsStore(const fs::path& tree, const fs::path& store) {
    fs::path t = fs::weakly_canonical(tree), s = fs::weakly_canonical(store);
    auto [ti, si] = std::mismatch(t.begin(), t.end(), s.begin(), s.end());
    return ti == t.end();
}

void parallelFor(size_t count, const char* phase, const std::function<void(size_t)>& fn) {
    std::atomic<size_t> next{0}, done{0};
    auto worker = [&] {
        for (size_t i; (i = next++) < count;) {
            fn(i);
            if (++done % 1000 == 0) progress(phase, done, count);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    progress(phase, done, count);
    std::fprintf(stderr, "\n");
}

}  // namespace ingest
