#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace commands {

namespace {

std::vector<cli::OptionSpec> specs() {
    return {
        {"--product", true, "<codename>", "Only this product's builds (wow, wowf, ...)"},
        {"--format", true, "<text|csv|json>", "Output format. Default: text"},
    };
}

const char* kUsage = "casc-tool builds <store-dir> [options]";

// Builds recorded before products were tracked carry no "product" in meta.json.
constexpr const char* kUnknownProduct = "?";

struct Row {
    std::string product, label, kind, fdids, missing, partial, tags, recorded;
};

size_t dataLines(const fs::path& tsv) {
    std::ifstream in(tsv);
    size_t n = 0;
    for (std::string line; std::getline(in, line);) n++;
    return n ? n - 1 : 0;  // header
}

Row describe(const store::Store& st, const std::string& label, const std::map<std::string, std::string>& tags) {
    auto meta = st.meta(label);
    auto get = [&](const char* key, const char* fallback) {
        auto it = meta.find(key);
        return it == meta.end() ? std::string(fallback) : it->second;
    };
    fs::path dir = st.buildDir(label);
    Row r;
    r.label = label;
    r.product = get("product", kUnknownProduct);
    r.kind = fs::exists(dir / "ROOT") ? "root" : fs::exists(dir / "files.tsv") ? "paths" : "incomplete";
    r.fdids = get("fdids", "-");  // import records don't count FDIDs
    r.missing = get("missing", "-");
    r.partial = std::to_string(dataLines(dir / "partial.tsv"));
    r.recorded = get("snapshot_at", "");
    if (r.recorded.empty()) r.recorded = get("imported_at", "-");
    for (const auto& [tag, target] : tags)
        if (target == label) r.tags += (r.tags.empty() ? "" : ",") + tag;
    return r;
}

}  // namespace

std::vector<cli::OptionSpec> buildsSpecs() { return specs(); }

void helpBuilds() {
    std::printf(
        "casc-tool builds -- list the builds a store holds, per product\n"
        "\n"
        "usage: %s\n"
        "\n"
        "  <store-dir>   a store filled by `snapshot` / `import`\n"
        "\n"
        "One row per build, grouped by product and oldest first within it -- each\n"
        "product (wow = _retail_, wowf = _forever_) is its own history, and\n"
        "`changes <build>` compares against the nearest root build above it in\n"
        "the same product. Product '?' marks\n"
        "records that predate product tracking. Reads only the store's own\n"
        "records (meta.json, partial.tsv, tags/), so it's instant.\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "example:\n"
        "  casc-tool builds /data/wow_store --product wowf\n");
}

int runBuilds(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(1, 1, kUsage);
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "csv" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text, csv, or json (got '%s')\n", fmt.c_str());
        return 2;
    }
    store::Store st(args.positionals()[0]);
    if (!fs::is_directory(st.root() / "builds")) {
        std::fprintf(stderr, "error: %s has no builds/ directory -- not a store\n", st.root().c_str());
        return 1;
    }
    auto only = args.option("--product");
    auto tags = st.tags();

    std::vector<Row> rows;
    for (const auto& label : st.builds()) {
        Row r = describe(st, label, tags);
        if (!only || r.product == *only) rows.push_back(std::move(r));
    }
    // builds() is already in label order; stable_sort keeps it within a product.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        bool ua = a.product == kUnknownProduct, ub = b.product == kUnknownProduct;
        return ua != ub ? ub : a.product < b.product;
    });

    if (fmt == "csv") {
        std::printf("product,label,kind,fdids,missing,partial,tags,recorded\n");
        for (const auto& r : rows)
            std::printf("%s,%s,%s,%s,%s,%s,%s,%s\n", format::csvEscape(r.product).c_str(),
                        format::csvEscape(r.label).c_str(), r.kind.c_str(), r.fdids.c_str(), r.missing.c_str(),
                        r.partial.c_str(), format::csvEscape(r.tags).c_str(), r.recorded.c_str());
    } else if (fmt == "json") {
        std::printf("[\n");
        for (size_t i = 0; i < rows.size(); i++) {
            const auto& r = rows[i];
            auto num = [](const std::string& v) { return v == "-" ? std::string("null") : v; };
            std::printf("%s{\"product\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\",\"fdids\":%s,\"missing\":%s,"
                        "\"partial\":%s,\"tags\":\"%s\",\"recorded\":\"%s\"}\n",
                        i ? "," : "", format::jsonEscape(r.product).c_str(), format::jsonEscape(r.label).c_str(),
                        r.kind.c_str(), num(r.fdids).c_str(), num(r.missing).c_str(), r.partial.c_str(),
                        format::jsonEscape(r.tags).c_str(), format::jsonEscape(r.recorded).c_str());
        }
        std::printf("]\n");
    } else {
        std::printf("%-8s %-24s %-6s %9s %8s %7s  %-20s %s\n", "product", "label", "kind", "fdids", "missing",
                    "partial", "recorded", "tags");
        for (const auto& r : rows)
            std::printf("%-8s %-24s %-6s %9s %8s %7s  %-20s %s\n", r.product.c_str(), r.label.c_str(),
                        r.kind.c_str(), r.fdids.c_str(), r.missing.c_str(), r.partial.c_str(), r.recorded.c_str(),
                        r.tags.c_str());
    }
    if (rows.empty()) std::fprintf(stderr, "no builds%s\n", only ? (" for product " + *only).c_str() : "");
    return 0;
}

}  // namespace commands
