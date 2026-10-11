#include "store.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <unistd.h>

#include "hashes/md5.h"

namespace fs = std::filesystem;

namespace store {

namespace {

std::atomic<uint64_t> stagingCounter{0};

std::string errnoText() { return std::strerror(errno); }

}  // namespace

fs::path Store::objectPath(const CKey& key) const {
    std::string h = rootmanifest::hex(key);
    return root_ / "objects" / h.substr(0, 2) / h.substr(2, 2) / h;
}

bool Store::hasObject(const CKey& key) const {
    std::error_code ec;
    return fs::exists(objectPath(key), ec);
}

fs::path Store::stagingPath() const {
    return root_ / "tmp" / (std::to_string(::getpid()) + "-" + std::to_string(stagingCounter++));
}

bool Store::commit(const CKey& key, const fs::path& staged, std::string* error) {
    auto actual = md5File(staged, error);
    if (!actual) return false;
    if (*actual != key) {
        fs::remove(staged);
        *error = "content hash mismatch: expected " + rootmanifest::hex(key) + ", got " + rootmanifest::hex(*actual);
        return false;
    }
    fs::path target = objectPath(key);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    // Read-only: a hardlinked view writing into an object would corrupt it for every build.
    fs::permissions(staged, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, ec);
    fs::rename(staged, target, ec);
    if (ec) {
        fs::remove(staged);
        *error = "couldn't move object into place at " + target.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool Store::putBytes(const CKey& key, const uint8_t* data, size_t size, std::string* error) {
    fs::path staged = stagingPath();
    std::error_code ec;
    fs::create_directories(staged.parent_path(), ec);
    std::ofstream out(staged, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        fs::remove(staged);
        *error = "couldn't write staging file " + staged.string();
        return false;
    }
    return commit(key, staged, error);
}

bool Store::cloneIn(const CKey& key, const fs::path& source, std::string* error) {
    fs::path staged = stagingPath();
    std::error_code ec;
    fs::create_directories(staged.parent_path(), ec);
    int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        *error = "couldn't open " + source.string() + ": " + errnoText();
        return false;
    }
    int out = ::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) {
        ::close(in);
        *error = "couldn't create " + staged.string() + ": " + errnoText();
        return false;
    }
    ssize_t n;
    while ((n = ::copy_file_range(in, nullptr, out, nullptr, 1 << 30, 0)) > 0) {
    }
    int copyErrno = errno;
    ::close(in);
    ::close(out);
    if (n < 0) {
        fs::remove(staged);
        *error = "copy_file_range " + source.string() + " -> " + staged.string() + ": " + std::strerror(copyErrno);
        return false;
    }
    return commit(key, staged, error);
}

// Caller (adoptFile) has just hashed `source` to `key`; see its contract.
bool Store::moveIn(const CKey& key, const fs::path& source, std::string* error) {
    fs::path target = objectPath(key);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        *error = "already stored";  // a duplicate copy; leave it where it is
        return false;
    }
    fs::create_directories(target.parent_path(), ec);
    fs::rename(source, target, ec);
    if (ec) {
        *error = "couldn't move " + source.string() + " to " + target.string() + ": " + ec.message();
        return false;
    }
    fs::permissions(target, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, ec);
    return true;
}

bool Store::recordRoot(const std::string& label, const std::vector<uint8_t>& root,
                       const std::vector<rootmanifest::Row>& rows, std::string* error) const {
    fs::path dir = buildDir(label);
    fs::path rootPath = dir / "ROOT";
    if (fs::exists(rootPath)) {
        std::ifstream in(rootPath, std::ios::binary);
        std::vector<uint8_t> existing{std::istreambuf_iterator<char>(in), {}};
        if (existing == root) return true;
        *error = dir.string() + " already holds a different ROOT -- pick another label";
        return false;
    }
    fs::create_directories(dir);
    if (!writeTextAtomic(rootPath, std::string(root.begin(), root.end())) ||
        !writeTextAtomic(dir / "manifest.tsv", rootmanifest::toTsv(rows))) {
        *error = "couldn't write ROOT/manifest.tsv under " + dir.string();
        return false;
    }
    return true;
}

std::vector<std::string> Store::builds() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_ / "builds", ec))
        if (entry.is_directory()) out.push_back(entry.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

std::map<std::string, std::string> Store::tags() const {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_ / "tags", ec)) {
        std::ifstream in(entry.path());
        std::string label;
        if (std::getline(in, label) && !label.empty()) out[entry.path().filename().string()] = label;
    }
    return out;
}

void Store::setTag(const std::string& name, const std::string& label) const {
    fs::create_directories(root_ / "tags");
    fs::path staged = root_ / "tags" / ("." + name + ".tmp");
    std::ofstream(staged) << label << "\n";
    fs::rename(staged, root_ / "tags" / name);  // atomic: a mount never sees a half-written tag
}

bool writeTextAtomic(const fs::path& path, const std::string& text) {
    fs::path staged = path.string() + ".tmp";
    std::ofstream out(staged, std::ios::binary);
    out << text;
    out.close();
    if (!out) return false;
    std::error_code ec;
    fs::rename(staged, path, ec);
    return !ec;
}

std::optional<CKey> md5File(const fs::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "couldn't open " + path.string() + " to hash it";
        return std::nullopt;
    }
    MD5_CTX ctx;
    MD5_Init(&ctx);
    std::vector<char> buffer(1 << 20);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
        MD5_Update(&ctx, buffer.data(), static_cast<unsigned long>(in.gcount()));
    CKey out;
    MD5_Final(out.data(), &ctx);
    return out;
}

}  // namespace store
