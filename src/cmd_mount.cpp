#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "listfile.hpp"
#include "rootmanifest.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

// Same locale snapshot fetched (cmd_snapshot.cpp kStoreLocale); a mismatch
// would list files whose objects were never stored.
constexpr uint32_t kMountLocale = CASC_LOCALE_ENUS;
constexpr uint32_t kNoDir = UINT32_MAX;
constexpr uint64_t kNoFd = UINT64_MAX;  // fi->fh for in-memory virtual files

std::vector<cli::OptionSpec> specs() {
    return {
        {"--listfile", true, "<path>",
         "Names the tree (FileDataID<->path). Default: listfile.csv (current directory). Remount to switch"},
        {"--foreground", false, "", "Stay attached to the terminal instead of daemonizing (logs to stderr)"},
    };
}

const char* kUsage = "casc-tool mount <store-dir> <mountpoint> [options]";

// Directory index built once from the listfile, shared by every build.
struct NameTree {
    struct Entry {
        std::string name;
        uint32_t id;  // child dir index, or fdid
        bool isDir;
    };
    struct Dir {
        uint32_t parent;
        std::vector<Entry> entries;  // sorted by name
    };
    std::vector<Dir> dirs{Dir{kNoDir, {}}};  // [0] = build root
    std::vector<uint32_t> dirOf;             // fdid -> containing dir, kNoDir if unnamed
    std::string source;

    const Entry* find(uint32_t dir, const std::string& name) const {
        const auto& e = dirs[dir].entries;
        auto it = std::lower_bound(e.begin(), e.end(), name, [](const Entry& a, const std::string& n) { return a.name < n; });
        return it != e.end() && it->name == name ? &*it : nullptr;
    }
};

NameTree buildNameTree(const std::map<unsigned, std::string>& names, const std::string& source) {
    NameTree t;
    t.source = source;
    unsigned maxFdid = names.empty() ? 0 : names.rbegin()->first;
    t.dirOf.assign(size_t(maxFdid) + 1, kNoDir);
    std::unordered_map<std::string, uint32_t> dirByPath{{"", 0}};
    for (const auto& [fdid, path] : names) {
        uint32_t dir = 0;
        size_t start = 0;
        for (size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1) {
            if (slash == start) continue;  // "//"
            auto [it, inserted] = dirByPath.emplace(path.substr(0, slash), uint32_t(t.dirs.size()));
            if (inserted) {
                t.dirs[dir].entries.push_back({path.substr(start, slash - start), it->second, true});
                t.dirs.push_back({dir, {}});
            }
            dir = it->second;
        }
        std::string leaf = path.substr(start);
        if (leaf.empty() || leaf == "." || leaf == "..") continue;
        t.dirs[dir].entries.push_back({leaf, fdid, false});
        t.dirOf[fdid] = dir;
    }
    for (auto& d : t.dirs) {
        // A name that is both a file and a directory (listfile noise) keeps the directory.
        std::stable_sort(d.entries.begin(), d.entries.end(), [](const auto& a, const auto& b) {
            return a.name != b.name ? a.name < b.name : a.isDir > b.isDir;
        });
        d.entries.erase(std::unique(d.entries.begin(), d.entries.end(),
                                    [](const auto& a, const auto& b) { return a.name == b.name; }),
                        d.entries.end());
    }
    return t;
}

// _by_fdid is lookup-only: listing ~1.9M entries took ~18 s per readdir,
// stalling file managers (Dolphin counts every visible folder's items) and
// doubling any recursive walk of a build. The README is its only listing.
constexpr const char* kByFdidReadmeName = "README.txt";
constexpr const char* kByFdidReadme =
    "Open _by_fdid/<FileDataID> directly, e.g. _by_fdid/1302850.\n"
    "Entries are not listed here: ~1.9M of them would stall file managers and\n"
    "double every recursive walk. Files with no listfile name are listed under\n"
    "_unresolved/.\n";

struct Build {
    std::once_flag once;
    std::string error;
    struct timespec mtime{};  // of builds/<label>/, shown on every directory of the build
    // ROOT builds are named by the mount's listfile, keyed by FDID. Path builds
    // (import without --root) carry their own names: ids are files.tsv rows,
    // so they have no _by_fdid and nothing is unresolved.
    const NameTree* tree = nullptr;
    std::unique_ptr<NameTree> ownTree;
    bool byFdid = false;
    std::unordered_map<uint32_t, store::CKey> present;  // ROOT: enUS selection minus missing.tsv
    std::vector<bool> dirUsed;                           // per NameTree dir: holds a present file
    std::vector<uint32_t> unresolved;                    // sorted, for _unresolved
};

struct Mount {
    store::Store st;
    NameTree names;
    struct timespec mtime;  // of the store root, for entries outside any build
    std::mutex buildsMutex;
    std::map<std::string, std::unique_ptr<Build>> builds;
};

Mount& ctx() { return *static_cast<Mount*>(fuse_get_context()->private_data); }

void loadRootBuild(Mount& m, const fs::path& dir, Build& b) {
    std::ifstream rootIn(dir / "ROOT", std::ios::binary);
    std::vector<uint8_t> root{std::istreambuf_iterator<char>(rootIn), {}};
    auto rows = rootmanifest::parse(root, &b.error);
    if (!b.error.empty()) {
        b.error = (dir / "ROOT").string() + ": " + b.error;
        return;
    }
    auto selected = rootmanifest::select(rows, kMountLocale);
    std::ifstream missing(dir / "missing.tsv");
    std::string line;
    std::getline(missing, line);  // header
    while (std::getline(missing, line)) selected.erase(uint32_t(std::stoul(line)));
    b.present = std::move(selected);
    b.tree = &m.names;
    b.byFdid = true;
}

// files.tsv: "path<TAB>ckey" per file, written by `import` without --root.
void loadPathBuild(const fs::path& dir, const std::string& label, Build& b) {
    std::ifstream in(dir / "files.tsv");
    std::map<unsigned, std::string> paths;
    std::string line;
    std::getline(in, line);  // header
    for (unsigned row = 0; std::getline(in, line); row++) {
        size_t tab = line.rfind('\t');
        if (tab == std::string::npos || line.size() - tab - 1 != 32) {
            b.error = (dir / "files.tsv").string() + " row " + std::to_string(row + 2) +
                      ": expected 'path<TAB>32-hex ckey', got '" + line + "'";
            return;
        }
        store::CKey key;
        for (size_t i = 0; i < 16; i++) key[i] = uint8_t(std::stoul(line.substr(tab + 1 + 2 * i, 2), nullptr, 16));
        paths[row] = line.substr(0, tab);
        b.present[row] = key;
    }
    b.ownTree = std::make_unique<NameTree>(buildNameTree(paths, "files.tsv of build " + label));
    b.tree = b.ownTree.get();
}

void loadBuild(Mount& m, const std::string& label, Build& b) {
    fs::path dir = m.st.buildDir(label);
    struct stat dirStat;
    if (::stat(dir.c_str(), &dirStat) == 0) b.mtime = dirStat.st_mtim;
    if (fs::exists(dir / "ROOT")) loadRootBuild(m, dir, b);
    else if (fs::exists(dir / "files.tsv")) loadPathBuild(dir, label, b);
    else b.error = dir.string() + " has neither ROOT nor files.tsv";
    if (!b.error.empty()) return;

    const NameTree& t = *b.tree;
    b.dirUsed.assign(t.dirs.size(), false);
    for (const auto& [id, _] : b.present) {
        uint32_t d = id < t.dirOf.size() ? t.dirOf[id] : kNoDir;
        if (d == kNoDir) {
            b.unresolved.push_back(id);
            continue;
        }
        for (; d != kNoDir && !b.dirUsed[d]; d = t.dirs[d].parent) b.dirUsed[d] = true;
    }
    std::sort(b.unresolved.begin(), b.unresolved.end());
    std::fprintf(stderr, "loaded build %s: %zu files (%zu unnamed by %s)\n", label.c_str(), b.present.size(),
                 b.unresolved.size(), t.source.c_str());
}

// Returns nullptr if `label` isn't a stored build, or it failed to load (logged).
Build* build(Mount& m, const std::string& label) {
    if (label.empty() || label[0] == '.' || !fs::is_directory(m.st.buildDir(label))) return nullptr;
    Build* b;
    {
        std::lock_guard<std::mutex> lock(m.buildsMutex);
        auto& slot = m.builds[label];
        if (!slot) slot = std::make_unique<Build>();
        b = slot.get();
    }
    std::call_once(b->once, [&] {
        loadBuild(m, label, *b);
        if (!b->error.empty()) std::fprintf(stderr, "error: build %s: %s\n", label.c_str(), b->error.c_str());
    });
    return b->error.empty() ? b : nullptr;
}

std::vector<std::string> splitPath(const char* path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char* p = path; *p; p++) {
        if (*p != '/') {
            cur += *p;
            continue;
        }
        if (!cur.empty()) parts.push_back(cur);
        cur.clear();
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

bool parseFdid(const std::string& s, uint32_t* out) {
    if (s.empty() || s.size() > 10 || !std::all_of(s.begin(), s.end(), ::isdigit)) return false;
    unsigned long v = std::stoul(s);
    if (v > UINT32_MAX) return false;
    *out = uint32_t(v);
    return true;
}

// What a path names. Layout (DESIGN.md "Browsing"):
//   /<tag> -> builds/<label>        /builds/<label>/<listfile path>
//   /builds/<label>/_by_fdid/<n>    /builds/<label>/_unresolved/<n>
//   /builds/<label>/.listfile       /builds/<label>/.missing
struct Node {
    enum Kind { None, Dir, Object, Link, Text, Real } kind = None;
    enum DirKind { Root, BuildsList, TreeDir, ByFdid, Unresolved } dirKind = Root;
    Build* build = nullptr;
    uint32_t dir = 0;
    store::CKey ckey{};
    std::string text;  // Link target or Text content
    fs::path real;
};

Node dirNode(Node::DirKind kind, Build* b = nullptr, uint32_t dir = 0) {
    Node n;
    n.kind = Node::Dir;
    n.dirKind = kind;
    n.build = b;
    n.dir = dir;
    return n;
}

Node objectNode(const store::CKey& ckey) {
    Node n;
    n.kind = Node::Object;
    n.ckey = ckey;
    return n;
}

Node textNode(Node::Kind kind, std::string text) {
    Node n;
    n.kind = kind;
    n.text = std::move(text);
    return n;
}

Node realNode(fs::path real) {
    Node n;
    n.kind = Node::Real;
    n.real = std::move(real);
    return n;
}

Node resolveInBuild(Mount& m, const std::string& label, Build& b, const std::vector<std::string>& rest) {
    if (rest.empty()) return dirNode(Node::TreeDir, &b, 0);
    const std::string& first = rest[0];
    const NameTree& t = *b.tree;
    if (rest.size() == 1 && first == ".listfile") return textNode(Node::Text, t.source + "\n");
    if (rest.size() == 1 && first == ".missing" && b.byFdid) return realNode(m.st.buildDir(label) / "missing.tsv");

    if ((first == "_by_fdid" && b.byFdid) || (first == "_unresolved" && !b.unresolved.empty())) {
        bool unresolvedOnly = first == "_unresolved";
        if (rest.size() == 1) return dirNode(unresolvedOnly ? Node::Unresolved : Node::ByFdid, &b);
        if (!unresolvedOnly && rest.size() == 2 && rest[1] == kByFdidReadmeName) return textNode(Node::Text, kByFdidReadme);
        uint32_t fdid;
        if (rest.size() != 2 || !parseFdid(rest[1], &fdid)) return {};
        auto it = b.present.find(fdid);
        if (it == b.present.end()) return {};
        bool named = fdid < t.dirOf.size() && t.dirOf[fdid] != kNoDir;
        if (unresolvedOnly && named) return {};
        return objectNode(it->second);
    }

    uint32_t dir = 0;
    for (size_t i = 0; i + 1 < rest.size(); i++) {
        const NameTree::Entry* e = t.find(dir, rest[i]);
        if (!e || !e->isDir || !b.dirUsed[e->id]) return {};
        dir = e->id;
    }
    const NameTree::Entry* leaf = t.find(dir, rest.back());
    if (!leaf) return {};
    if (leaf->isDir) return b.dirUsed[leaf->id] ? dirNode(Node::TreeDir, &b, leaf->id) : Node{};
    auto it = b.present.find(leaf->id);
    return it == b.present.end() ? Node{} : objectNode(it->second);
}

Node resolve(Mount& m, const char* path) {
    auto parts = splitPath(path);
    if (parts.empty()) return dirNode(Node::Root);
    if (parts[0] != "builds") {
        auto tags = m.st.tags();
        auto t = tags.find(parts[0]);
        if (parts.size() != 1 || t == tags.end()) return {};
        return textNode(Node::Link, "builds/" + t->second);
    }
    if (parts.size() == 1) return dirNode(Node::BuildsList);
    Build* b = build(m, parts[1]);
    if (!b) return {};
    return resolveInBuild(m, parts[1], *b, std::vector<std::string>(parts.begin() + 2, parts.end()));
}

int fsGetattr(const char* path, struct stat* st, struct fuse_file_info*) {
    Mount& m = ctx();
    Node n = resolve(m, path);
    std::memset(st, 0, sizeof(*st));
    st->st_uid = ::getuid();
    st->st_gid = ::getgid();
    st->st_mtim = st->st_ctim = n.build ? n.build->mtime : m.mtime;
    switch (n.kind) {
        case Node::None: return -ENOENT;
        case Node::Dir:
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
            return 0;
        case Node::Link:
            st->st_mode = S_IFLNK | 0777;
            st->st_nlink = 1;
            st->st_size = off_t(n.text.size());
            return 0;
        case Node::Text:
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = off_t(n.text.size());
            return 0;
        case Node::Object:
        case Node::Real: {
            struct stat real;
            fs::path p = n.kind == Node::Object ? m.st.objectPath(n.ckey) : n.real;
            if (::stat(p.c_str(), &real) != 0) return -ENOENT;  // object listed but absent: interrupted snapshot
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = real.st_size;
            st->st_blocks = real.st_blocks;
            st->st_mtim = real.st_mtim;
            return 0;
        }
    }
    return -ENOENT;
}

int fsReadlink(const char* path, char* buf, size_t size) {
    Node n = resolve(ctx(), path);
    if (n.kind != Node::Link) return -EINVAL;
    std::snprintf(buf, size, "%s", n.text.c_str());
    return 0;
}

int fsReaddir(const char* path, void* buf, fuse_fill_dir_t fill, off_t, struct fuse_file_info*,
              enum fuse_readdir_flags) {
    Mount& m = ctx();
    Node n = resolve(m, path);
    if (n.kind != Node::Dir) return -ENOTDIR;
    auto add = [&](const std::string& name) { fill(buf, name.c_str(), nullptr, 0, fuse_fill_dir_flags(0)); };
    add(".");
    add("..");
    switch (n.dirKind) {
        case Node::Root:
            add("builds");
            for (const auto& [tag, _] : m.st.tags()) add(tag);
            break;
        case Node::BuildsList:
            for (const auto& label : m.st.builds()) add(label);
            break;
        case Node::ByFdid:
            add(kByFdidReadmeName);
            break;
        case Node::Unresolved:
            for (uint32_t f : n.build->unresolved) add(std::to_string(f));
            break;
        case Node::TreeDir:
            if (n.dir == 0) {
                add(".listfile");
                if (n.build->byFdid) add("_by_fdid");
                if (n.build->byFdid) add(".missing");
                if (!n.build->unresolved.empty()) add("_unresolved");
            }
            for (const auto& e : n.build->tree->dirs[n.dir].entries)
                if (e.isDir ? n.build->dirUsed[e.id] : n.build->present.count(e.id) > 0) add(e.name);
            break;
    }
    return 0;
}

int fsOpen(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    Mount& m = ctx();
    Node n = resolve(m, path);
    if (n.kind == Node::Text) {
        fi->fh = kNoFd;
        return 0;
    }
    if (n.kind != Node::Object && n.kind != Node::Real) return n.kind == Node::None ? -ENOENT : -EISDIR;
    fs::path p = n.kind == Node::Object ? m.st.objectPath(n.ckey) : n.real;
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    fi->fh = uint64_t(fd);
    fi->keep_cache = n.kind == Node::Object;  // objects are immutable (invariant 2)
    return 0;
}

int fsRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    if (fi->fh == kNoFd) {
        Node n = resolve(ctx(), path);
        if (offset >= off_t(n.text.size())) return 0;
        size_t count = std::min(size, n.text.size() - size_t(offset));
        std::memcpy(buf, n.text.data() + offset, count);
        return int(count);
    }
    ssize_t r = ::pread(int(fi->fh), buf, size, offset);
    return r < 0 ? -errno : int(r);
}

int fsRelease(const char*, struct fuse_file_info* fi) {
    if (fi->fh != kNoFd) ::close(int(fi->fh));
    return 0;
}

void* fsInit(struct fuse_conn_info*, struct fuse_config* cfg) {
    // Store content never changes, but tags and the builds list do when a
    // snapshot runs alongside the mount: a minute of kernel caching bounds
    // how stale those look.
    cfg->entry_timeout = 60;
    cfg->attr_timeout = 60;
    cfg->negative_timeout = 10;
    return fuse_get_context()->private_data;
}

}  // namespace

std::vector<cli::OptionSpec> mountSpecs() { return specs(); }

void helpMount() {
    std::printf(
        "casc-tool mount -- browse every build in a store as a read-only filesystem\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <store-dir>    a store filled by `snapshot`\n"
        "  <mountpoint>   an empty directory you own\n"
        "\n"
        "Layout: <mnt>/live -> builds/<label> (one link per tag: live, ptr, beta),\n"
        "<mnt>/builds/<label>/<listfile path>, plus _by_fdid/<id> (stable whatever\n"
        "the listfile says), _unresolved/<id>, .listfile (what named this tree)\n"
        "and .missing (files the store doesn't have, and why). enUS files only.\n"
        "\n"
        "No root needed: mounting goes through fusermount3. Unmount with\n"
        "`fusermount3 -u <mountpoint>`. A new listfile is just a remount.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "example:\n"
        "  casc-tool mount /data/wow_store ~/wow --listfile community-listfile-2026-10-08.csv\n");
}

int runMount(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(2, 2, kUsage);
    fs::path storeDir = args.positionals()[0];
    std::string mountpoint = args.positionals()[1];
    std::string listFile = args.optionOr("--listfile", "listfile.csv");

    if (!fs::is_directory(storeDir / "builds")) {
        std::fprintf(stderr, "error: %s has no builds/ directory -- run `casc-tool snapshot` into it first\n",
                     storeDir.string().c_str());
        return 1;
    }
    std::string error;
    auto names = listfile::load(listFile, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    struct stat rootStat {};
    ::stat(storeDir.c_str(), &rootStat);
    Mount mount{store::Store(fs::absolute(storeDir)), buildNameTree(names, fs::absolute(listFile).string()),
                rootStat.st_mtim, {}, {}};
    names.clear();

    std::string tagSummary;
    for (const auto& [tag, label] : mount.st.tags()) tagSummary += " " + tag + "->" + label;
    std::fprintf(stderr, "mounting %s at %s: %zu builds, tags:%s; %zu directories named by %s\n",
                 storeDir.string().c_str(), mountpoint.c_str(), mount.st.builds().size(),
                 tagSummary.empty() ? " none" : tagSummary.c_str(), mount.names.dirs.size(), listFile.c_str());

    struct fuse_operations ops {};
    ops.getattr = fsGetattr;
    ops.readlink = fsReadlink;
    ops.readdir = fsReaddir;
    ops.open = fsOpen;
    ops.read = fsRead;
    ops.release = fsRelease;
    ops.init = fsInit;

    std::string fsname = "fsname=casc-store:" + mount.st.root().string();
    std::vector<std::string> argvStore = {"casc-tool", mountpoint, "-o", "ro,subtype=casc," + fsname};
    if (args.flag("--foreground")) argvStore.push_back("-f");
    std::vector<char*> argv;
    for (auto& a : argvStore) argv.push_back(a.data());
    return fuse_main(int(argv.size()), argv.data(), &ops, &mount);
}

}  // namespace commands
