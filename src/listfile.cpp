#include "listfile.hpp"

#include <fstream>

namespace listfile {

std::map<unsigned, std::string> load(const std::string& path, std::string* error) {
    std::map<unsigned, std::string> entries;
    std::ifstream in(path);
    if (!in) {
        *error = "couldn't open '" + path + "'";
        return entries;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto sep = line.find(';');
        if (sep == std::string::npos) continue;
        try {
            unsigned id = static_cast<unsigned>(std::stoul(line.substr(0, sep)));
            std::string name = line.substr(sep + 1);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            entries[id] = name;
        } catch (const std::exception&) {
            continue;
        }
    }
    return entries;
}

std::vector<Change> diff(const std::map<unsigned, std::string>& a, const std::map<unsigned, std::string>& b) {
    std::vector<Change> changes;

    for (const auto& [id, name] : b) {
        auto it = a.find(id);
        if (it == a.end()) {
            changes.push_back({id, 'A', "", name});
        } else if (it->second != name) {
            changes.push_back({id, 'C', it->second, name});
        }
    }
    for (const auto& [id, name] : a) {
        if (b.find(id) == b.end()) changes.push_back({id, 'R', name, ""});
    }

    return changes;
}

std::vector<unsigned> loadIdList(const std::string& path, std::string* error) {
    std::vector<unsigned> ids;
    std::ifstream in(path);
    if (!in) {
        *error = "couldn't open '" + path + "'";
        return ids;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            size_t consumed = 0;
            unsigned id = static_cast<unsigned>(std::stoul(line, &consumed));
            if (consumed != line.size()) continue;  // trailing junk -- not a bare ID
            ids.push_back(id);
        } catch (const std::exception&) {
            continue;
        }
    }
    return ids;
}

}  // namespace listfile
