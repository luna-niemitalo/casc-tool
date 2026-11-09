#include <fnmatch.h>

#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <CascLib.h>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "listfile.hpp"
#include "rootmanifest.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

// Same locale snapshot stored (cmd_snapshot.cpp kStoreLocale), so a change
// listed here is a change in what the store and mount serve.
constexpr uint32_t kStoreLocale = CASC_LOCALE_ENUS;

std::vector<cli::OptionSpec> specs() {
    return {
        {"--status", true, "<list>",
         "Comma-separated statuses to list: added, removed, changed, unchanged. Default: added,removed,changed "
         "(all four are always counted)"},
        {"--listfile", true, "<path>", "Name each FDID by this listfile (and allow --mask). Default: FDIDs only"},
        {"--mask", true, "<glob>",
         "Only FDIDs whose listfile path matches (fnmatch, case-insensitive, '*' crosses '/'); needs --listfile"},
        {"--cross-product", false, "",
         "Allow comparing builds of different products (e.g. wow vs wowf): normally refused, since each "
         "product is its own history"},
        {"--format", true, "<text|csv|json>", "Output format. Default: text. csv/json include both object paths"},
    };
}

const char* kUsage = "casc-tool changes <store-dir> [<older>] <newer> [options]";

const std::map<std::string, rootmanifest::Change>& statusNames() {
    static const std::map<std::string, rootmanifest::Change> names = {
        {"added", rootmanifest::Change::Added},
        {"removed", rootmanifest::Change::Removed},
        {"changed", rootmanifest::Change::Changed},
        {"unchanged", rootmanifest::Change::Unchanged},
    };
    return names;
}

std::string statusName(rootmanifest::Change c) {
    for (const auto& [name, value] : statusNames())
        if (value == c) return name;
    return "?";
}

// IN, non-critical (filters output only): --status. Each name checked on its own.
std::optional<std::set<rootmanifest::Change>> parseStatuses(const std::string& list) {
    std::set<rootmanifest::Change> out;
    std::stringstream ss(list);
    for (std::string name; std::getline(ss, name, ',');) {
        auto it = statusNames().find(name);
        if (it == statusNames().end()) {
            std::fprintf(stderr, "error: --status: expected added, removed, changed or unchanged, got '%s'\n",
                         name.c_str());
            return std::nullopt;
        }
        out.insert(it->second);
    }
    return out;
}

std::string productOf(const store::Store& st, const std::string& label) {
    auto meta = st.meta(label);
    auto it = meta.find("product");
    return it == meta.end() ? "" : it->second;
}

// The newest ROOT build of `product` ordered before `label` (store::labelLess).
std::optional<std::string> previousBuild(const store::Store& st, const std::string& label, const std::string& product) {
    std::optional<std::string> prev;
    for (const auto& b : st.builds()) {
        if (!store::labelLess(b, label)) break;
        if (productOf(st, b) == product && fs::exists(st.buildDir(b) / "ROOT")) prev = b;
    }
    return prev;
}

std::string objectOrEmpty(const store::Store& st, const store::CKey& key, bool present) {
    return present && st.hasObject(key) ? st.objectPath(key).string() : "";
}

}  // namespace

std::vector<cli::OptionSpec> changesSpecs() { return specs(); }

void helpChanges() {
    std::printf(
        "casc-tool changes -- which files a build added, removed or changed\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <store-dir>   a store filled by `snapshot` / `import`\n"
        "  <older>       a build label or tag. Omitted: the previous build of\n"
        "                <newer>'s own product (see `builds`)\n"
        "  <newer>       a build label or tag, e.g. live, forever, 12.1.5.70783\n"
        "\n"
        "Compares the two builds' ROOTs per FDID (the enUS file the store\n"
        "serves): content is the same exactly when the CKey is. Each product\n"
        "(wow = _retail_, wowf = _forever_) is its own history, so a pair from\n"
        "different products is refused unless --cross-product. Path builds\n"
        "(import without --root) have no FDIDs and can't be compared.\n"
        "\n"
        "Counts go to stderr; listed rows to stdout. csv/json carry each side's\n"
        "object path (empty where the store lacks the object -- see the build's\n"
        "missing.tsv), so content tools can diff the bytes directly.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool changes /data/wow_store live                 # live vs the wow build before it\n"
        "  casc-tool changes /data/wow_store 1.60.1.70790 forever --listfile lf.csv --mask 'dbfilesclient/*'\n"
        "  casc-tool changes /data/wow_store live forever --cross-product --status unchanged --format csv\n");
}

int runChanges(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(2, 3, kUsage);
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "csv" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text, csv, or json (got '%s')\n", fmt.c_str());
        return 2;
    }
    auto statuses = parseStatuses(args.optionOr("--status", "added,removed,changed"));
    if (!statuses) return 2;
    auto mask = args.option("--mask");
    auto listFile = args.option("--listfile");
    if (mask && !listFile) {
        std::fprintf(stderr, "error: --mask matches listfile paths, so it needs --listfile\n");
        return 2;
    }

    const auto& pos = args.positionals();
    store::Store st(pos[0]);
    auto resolveOrFail = [&](const std::string& name) {
        auto label = st.resolve(name);
        if (!label) std::fprintf(stderr, "error: '%s' is neither a build nor a tag in %s (see `casc-tool builds`)\n",
                                 name.c_str(), st.root().c_str());
        return label;
    };
    auto newer = resolveOrFail(pos.back());
    if (!newer) return 1;
    std::string newerProduct = productOf(st, *newer);

    std::optional<std::string> older;
    if (pos.size() == 3) {
        older = resolveOrFail(pos[1]);
        if (!older) return 1;
    } else {
        if (newerProduct.empty()) {
            std::fprintf(stderr, "error: build %s records no product, so it has no 'previous build' -- name the "
                         "older build explicitly\n", newer->c_str());
            return 1;
        }
        older = previousBuild(st, *newer, newerProduct);
        if (!older) {
            std::fprintf(stderr, "error: %s is the oldest stored %s build -- nothing to compare it with\n",
                         newer->c_str(), newerProduct.c_str());
            return 1;
        }
    }
    std::string olderProduct = productOf(st, *older);
    if (!olderProduct.empty() && !newerProduct.empty() && olderProduct != newerProduct &&
        !args.flag("--cross-product")) {
        std::fprintf(stderr, "error: %s is a %s build and %s a %s build -- separate histories; pass "
                     "--cross-product to compare them anyway\n",
                     older->c_str(), olderProduct.c_str(), newer->c_str(), newerProduct.c_str());
        return 2;
    }
    if (olderProduct.empty() || newerProduct.empty())
        std::fprintf(stderr, "warning: %s records no product; can't check both builds are the same product\n",
                     (olderProduct.empty() ? *older : *newer).c_str());

    std::string error;
    auto olderRows = st.readRoot(*older, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    auto newerRows = st.readRoot(*newer, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    auto diffs = rootmanifest::compare(rootmanifest::select(olderRows, kStoreLocale),
                                       rootmanifest::select(newerRows, kStoreLocale));
    olderRows.clear();
    newerRows.clear();

    std::map<unsigned, std::string> names;
    if (listFile) {
        names = listfile::load(*listFile, &error);
        if (!error.empty()) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }
    auto nameOf = [&](uint32_t fdid) {
        auto it = names.find(fdid);
        return it == names.end() ? std::string() : it->second;
    };

    std::map<rootmanifest::Change, size_t> counts;
    if (fmt == "csv") std::printf("fdid,status,ckey_old,ckey_new,object_old,object_new,path\n");
    if (fmt == "json") std::printf("[\n");
    size_t listed = 0;
    for (const auto& d : diffs) {
        std::string path = nameOf(d.fdid);
        if (mask && fnmatch(mask->c_str(), path.c_str(), FNM_CASEFOLD) != 0) continue;
        counts[d.change]++;
        if (!statuses->count(d.change)) continue;
        bool hasOld = d.change != rootmanifest::Change::Added, hasNew = d.change != rootmanifest::Change::Removed;
        std::string ckeyOld = hasOld ? rootmanifest::hex(d.a) : "", ckeyNew = hasNew ? rootmanifest::hex(d.b) : "";
        if (fmt == "csv") {
            std::printf("%u,%s,%s,%s,%s,%s,%s\n", d.fdid, statusName(d.change).c_str(), ckeyOld.c_str(),
                        ckeyNew.c_str(), objectOrEmpty(st, d.a, hasOld).c_str(),
                        objectOrEmpty(st, d.b, hasNew).c_str(), format::csvEscape(path).c_str());
        } else if (fmt == "json") {
            std::printf("%s{\"fdid\":%u,\"status\":\"%s\",\"ckey_old\":\"%s\",\"ckey_new\":\"%s\","
                        "\"object_old\":\"%s\",\"object_new\":\"%s\",\"path\":\"%s\"}\n",
                        listed ? "," : "", d.fdid, statusName(d.change).c_str(), ckeyOld.c_str(), ckeyNew.c_str(),
                        format::jsonEscape(objectOrEmpty(st, d.a, hasOld)).c_str(),
                        format::jsonEscape(objectOrEmpty(st, d.b, hasNew)).c_str(),
                        format::jsonEscape(path).c_str());
        } else {
            std::printf("%c %u%s%s\n", char(d.change), d.fdid, path.empty() ? "" : " ", path.c_str());
        }
        listed++;
    }
    if (fmt == "json") std::printf("]\n");

    using C = rootmanifest::Change;
    std::fprintf(stderr, "%s (%s) -> %s (%s)%s: %zu added, %zu removed, %zu changed, %zu unchanged; %zu listed\n",
                 older->c_str(), olderProduct.empty() ? "?" : olderProduct.c_str(), newer->c_str(),
                 newerProduct.empty() ? "?" : newerProduct.c_str(), mask ? (" [" + *mask + "]").c_str() : "",
                 counts[C::Added], counts[C::Removed], counts[C::Changed], counts[C::Unchanged], listed);
    return 0;
}

}  // namespace commands
