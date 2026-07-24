#pragma once

#include <cstdio>
#include <string>

// Tiny output-escaping helpers shared by any command offering --format
// csv/json. Deliberately minimal: WoW paths are a known, narrow character
// set (no need for full RFC 4180 generality for csvEscape) -- but jsonEscape
// still has to satisfy RFC 8259, which requires every C0 control character
// (U+0000-U+001F) to be escaped, not just the ones WoW paths happen to use.
namespace format {

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string csvEscape(const std::string& s) {
    bool needsQuoting = s.find_first_of(",\"\n") != std::string::npos;
    if (!needsQuoting) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

}  // namespace format
