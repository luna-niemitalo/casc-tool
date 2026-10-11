#include "rootmanifest.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rootmanifest {

namespace {

constexpr uint32_t kNoNameHash = 0x10000000;
constexpr uint32_t kLowViolence = 0x80;
constexpr uint32_t kDontLoad = 0x100;

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

}  // namespace

std::vector<Row> parse(const std::vector<uint8_t>& root, std::string* error) {
    if (root.size() < 24 || std::memcmp(root.data(), "TSFM", 4) != 0 || u32(root, 8) != 2) {
        *error = "expected an MFST ('TSFM') version 2 ROOT, got " + std::to_string(root.size()) + " bytes" +
                 (root.size() >= 12 ? ", version " + std::to_string(u32(root, 8)) : "");
        return {};
    }
    std::vector<Row> rows;
    rows.reserve(u32(root, 12));  // header's total file count
    size_t at = u32(root, 4);     // header size
    size_t block = 0;
    while (at < root.size()) {
        // v2 block header: count, locale, content flags split over u32+u32+u8 (CascLib's
        // FILE_ROOT_GROUP_HEADER_58221: flags1 | flags2 | flags3 << 17).
        if (at + 17 > root.size()) {
            *error = "block " + std::to_string(block) + " header at " + std::to_string(at) + " runs past end " +
                     std::to_string(root.size());
            return {};
        }
        uint32_t count = u32(root, at);
        uint32_t locale = u32(root, at + 4);
        uint32_t content = u32(root, at + 8) | u32(root, at + 12) | (uint32_t(root[at + 16]) << 17);
        at += 17;
        size_t idsAt = at, keysAt = at + 4 * size_t(count);
        at = keysAt + 16 * size_t(count) + ((content & kNoNameHash) ? 0 : 8 * size_t(count));
        if (at > root.size()) {
            *error = "block " + std::to_string(block) + " (" + std::to_string(count) + " records) ends at " +
                     std::to_string(at) + ", past file size " + std::to_string(root.size());
            return {};
        }
        int64_t fdid = -1;
        for (uint32_t i = 0; i < count; i++) {
            fdid += int64_t(int32_t(u32(root, idsAt + 4 * i))) + 1;  // delta-coded
            Row row{uint32_t(fdid), locale, content, {}};
            std::memcpy(row.ckey.data(), root.data() + keysAt + 16 * i, 16);
            rows.push_back(row);
        }
        block++;
    }
    return rows;
}

std::unordered_map<uint32_t, CKey> select(const std::vector<Row>& rows, uint32_t localeMask) {
    std::unordered_map<uint32_t, CKey> chosen;
    chosen.reserve(rows.size() / 2);
    for (uint32_t audio : {0u, 1u}) {
        for (const auto& r : rows) {
            if (r.contentFlags & (kDontLoad | kLowViolence)) continue;
            if ((r.contentFlags >> 31) != audio) continue;
            if (r.localeFlags != 0 && (r.localeFlags & localeMask) == 0) continue;
            chosen.emplace(r.fdid, r.ckey);  // first hit wins
        }
    }
    return chosen;
}

std::string toTsv(const std::vector<Row>& rows) {
    std::vector<const Row*> sorted;
    for (const auto& r : rows) sorted.push_back(&r);
    std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->fdid < b->fdid; });
    std::string tsv = "fdid\tlocale\tcontent_flags\tckey\n";
    char line[96];
    for (auto* r : sorted) {
        std::snprintf(line, sizeof(line), "%u\t%08x\t%08x\t", r->fdid, r->localeFlags, r->contentFlags);
        tsv += line + hex(r->ckey) + "\n";
    }
    return tsv;
}

std::string hex(const CKey& key) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t c : key) {
        s += digits[c >> 4];
        s += digits[c & 0xF];
    }
    return s;
}

}  // namespace rootmanifest
