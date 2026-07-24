#include "cli.hpp"

#include <algorithm>
#include <cstdio>

namespace cli {

Args::Args(const std::vector<std::string>& argv, const std::vector<OptionSpec>& specs) {
    auto findSpec = [&](const std::string& name) -> const OptionSpec* {
        for (const auto& spec : specs) {
            if (spec.name == name) return &spec;
        }
        return nullptr;
    };

    for (size_t i = 0; i < argv.size(); i++) {
        const std::string& tok = argv[i];

        if (tok.size() < 2 || tok[0] != '-' || tok[1] != '-') {
            positionals_.push_back(tok);
            continue;
        }

        std::string name = tok;
        std::string inlineValue;
        bool hasInlineValue = false;
        auto eq = tok.find('=');
        if (eq != std::string::npos) {
            name = tok.substr(0, eq);
            inlineValue = tok.substr(eq + 1);
            hasInlineValue = true;
        }

        const OptionSpec* spec = findSpec(name);
        if (!spec) {
            throw ArgError("unknown option '" + name + "' (see --help)");
        }

        if (!spec->takesValue) {
            if (hasInlineValue) {
                throw ArgError("option '" + name + "' doesn't take a value");
            }
            flags_.insert(name);
            continue;
        }

        if (hasInlineValue) {
            values_[name] = inlineValue;
            continue;
        }

        if (i + 1 >= argv.size()) {
            throw ArgError("option '" + name + "' needs a value (" + spec->valueHint + ")");
        }
        values_[name] = argv[++i];
    }
}

bool Args::flag(const std::string& name) const {
    return flags_.count(name) > 0;
}

std::optional<std::string> Args::option(const std::string& name) const {
    auto it = values_.find(name);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::string Args::optionOr(const std::string& name, const std::string& fallback) const {
    return option(name).value_or(fallback);
}

void Args::requirePositionals(size_t minCount, long maxCount, const std::string& usage) const {
    size_t n = positionals_.size();
    bool tooFew = n < minCount;
    bool tooMany = maxCount >= 0 && n > static_cast<size_t>(maxCount);
    if (tooFew || tooMany) {
        throw ArgError("wrong number of arguments\nusage: " + usage);
    }
}

void printOptionTable(const std::vector<OptionSpec>& specs) {
    size_t width = 0;
    for (const auto& spec : specs) {
        size_t w = spec.name.size() + 1 + spec.valueHint.size();
        width = std::max(width, w);
    }
    for (const auto& spec : specs) {
        std::string left = spec.name;
        if (!spec.valueHint.empty()) left += " " + spec.valueHint;
        std::printf("  %-*s  %s\n", static_cast<int>(width), left.c_str(), spec.help.c_str());
    }
}

}  // namespace cli
