#pragma once

#include <string>

// Tiny output-escaping helpers shared by any command offering --format
// csv/json. Deliberately minimal: WoW paths are a known, narrow character
// set (no need for full RFC 4180 / RFC 8259 generality here).
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
            default: out += c;
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
