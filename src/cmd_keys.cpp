#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cli.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "keys.hpp"
#include "storage.hpp"

namespace commands {

namespace {

// By FileDataID, so harvesting never depends on --listfile naming these.
constexpr const char* kTactKeyFdid = "1302850";        // dbfilesclient/tactkey.db2
constexpr const char* kTactKeyLookupFdid = "1302851";  // dbfilesclient/tactkeylookup.db2

std::vector<cli::OptionSpec> specs() {
    auto s = storage::commonOptionSpecs();
    s.push_back({"--dbcache", true, "<path>",
                 "Hotfix cache to read. Default: <install>/_retail_/Cache/ADB/enUS/DBCache.bin, where <install> "
                 "is --storage (or its parent, if --storage is the Data directory)"});
    s.push_back({"--format", true, "<text|csv|json>",
                 "Output format. Default: text (only NEW keys, as 'NAME KEY' lines -- appendable to a keys file); "
                 "csv/json list every harvested name with its status"});
    return s;
}

const char* kUsage = "casc-tool keys [options]";

std::string defaultDbCache(const std::string& storagePath) {
    return (storage::installRoot(storagePath) / "_retail_" / "Cache" / "ADB" / "enUS" / "DBCache.bin").string();
}

const char* statusName(keys::Status s) {
    switch (s) {
        case keys::Status::New: return "new";
        case keys::Status::Known: return "known";
        case keys::Status::NameOnly: return "name-only";
    }
    return "?";
}

}  // namespace

std::vector<cli::OptionSpec> keysSpecs() { return specs(); }

void helpKeys() {
    std::printf(
        "casc-tool keys -- harvest TACT decryption keys from game data and your hotfix cache\n"
        "\n"
        "usage: %s\n"
        "\n"
        "Reads every place a client receives keys: tactkey.db2 + tactkeylookup.db2\n"
        "from the storage, plus TactKey/TactKeyLookup hotfix rows and BroadcastText\n"
        "key payloads from DBCache.bin. Compares the result against --keys: a key\n"
        "is 'new' if its name isn't there yet. 'name-only' means the client knows\n"
        "a key's name but hasn't been sent its value -- content still locked.\n"
        "\n"
        "Hotfix keys only ever exist in your local cache, so re-run this after\n"
        "playing (new content unlocks push new keys).\n"
        "\n"
        "options:\n",
        kUsage);
    cli::printOptionTable(specs());
    std::printf(
        "\n"
        "examples:\n"
        "  casc-tool keys --storage /games/wow/Data --keys WoW.txt            # new keys only\n"
        "  casc-tool keys --storage /games/wow/Data --keys WoW.txt >> my-keys.txt\n"
        "  casc-tool keys --storage /games/wow/Data --keys WoW.txt --format csv  # everything\n");
}

int runKeys(const std::vector<std::string>& rawArgs) {
    cli::Args args(rawArgs, specs());
    args.requirePositionals(0, 0, kUsage);
    std::string fmt = args.optionOr("--format", "text");
    if (fmt != "text" && fmt != "csv" && fmt != "json") {
        std::fprintf(stderr, "error: --format must be text, csv, or json (got '%s')\n", fmt.c_str());
        return 2;
    }
    auto opts = storage::fromArgs(args);
    std::string dbCachePath = args.optionOr("--dbcache", defaultDbCache(opts.path));

    std::string error;
    std::set<std::string> known;
    if (opts.keysFile) {
        known = keys::loadKnownNames(*opts.keysFile, &error);
        if (!error.empty()) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    } else {
        std::fprintf(stderr, "warning: no --keys given, so every harvested key counts as new\n");
    }

    storage::StorageHandle hStorage;
    if (!storage::open(opts, hStorage)) return 1;

    keys::Bytes keyDb2, lookupDb2;
    if (!storage::readFile(hStorage.get(), kTactKeyFdid, "", keyDb2, &error) ||
        !storage::readFile(hStorage.get(), kTactKeyLookupFdid, "", lookupDb2, &error)) {
        std::fprintf(stderr, "error: reading key tables from storage: %s\n", error.c_str());
        return 1;
    }
    auto db2Keys = keys::parseKeyTable(keyDb2, keys::kTactKeyTableHash, 16, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: tactkey.db2 (FDID %s): %s\n", kTactKeyFdid, error.c_str());
        return 1;
    }
    auto db2Names = keys::parseKeyTable(lookupDb2, keys::kTactKeyLookupTableHash, 8, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: tactkeylookup.db2 (FDID %s): %s\n", kTactKeyLookupFdid, error.c_str());
        return 1;
    }

    std::ifstream cacheIn(dbCachePath, std::ios::binary);
    if (!cacheIn) {
        std::fprintf(stderr, "error: couldn't open hotfix cache '%s' (see --dbcache)\n", dbCachePath.c_str());
        return 1;
    }
    keys::Bytes cacheBytes{std::istreambuf_iterator<char>(cacheIn), {}};
    auto cache = keys::parseDbCache(cacheBytes, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s: %s\n", dbCachePath.c_str(), error.c_str());
        return 1;
    }
    std::fprintf(stderr, "read %zu key rows, %zu name rows from DB2; %zu hotfix entries from %s\n",
                 db2Keys.size(), db2Names.size(), cache.entries, dbCachePath.c_str());

    auto rows = keys::harvest(db2Keys, db2Names, cache, known);

    if (fmt == "csv") {
        std::printf("name,key,status,source\n");
        for (const auto& r : rows)
            std::printf("%s,%s,%s,%s\n", r.name.c_str(), r.key.c_str(), statusName(r.status),
                        format::csvEscape(r.source).c_str());
    } else if (fmt == "json") {
        std::printf("[\n");
        for (size_t i = 0; i < rows.size(); i++) {
            const auto& r = rows[i];
            std::printf("%s{\"name\":\"%s\",\"key\":\"%s\",\"status\":\"%s\",\"source\":\"%s\"}\n", i > 0 ? "," : "",
                        r.name.c_str(), r.key.c_str(), statusName(r.status), format::jsonEscape(r.source).c_str());
        }
        std::printf("]\n");
    } else {
        for (const auto& r : rows)
            if (r.status == keys::Status::New) std::printf("%s %s\n", r.name.c_str(), r.key.c_str());
    }

    size_t fresh = 0, knownCount = 0, nameOnly = 0;
    for (const auto& r : rows) {
        if (r.status == keys::Status::New) fresh++;
        else if (r.status == keys::Status::Known) knownCount++;
        else nameOnly++;
    }
    std::fprintf(stderr, "%zu new, %zu already known, %zu name-only (value not sent yet)\n", fresh, knownCount,
                 nameOnly);
    return 0;
}

}  // namespace commands
