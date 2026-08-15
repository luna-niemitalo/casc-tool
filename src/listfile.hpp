#pragma once

#include <map>
#include <string>
#include <vector>

// Pure listfile parsing/comparison logic, deliberately independent of any
// CASC storage. Split out of cmd_diff.cpp so it's directly unit-testable
// without needing a CLI invocation or a compiled storage.
namespace listfile {

// Parses a wow-listfile-style CSV: "FileDataId;FullFileName" per line.
// Malformed lines (no ';', non-numeric ID) are silently skipped rather than
// aborting -- these files can be multi-million lines and a single bad line
// shouldn't take down the whole parse. Sets *error and returns an empty map
// if the file itself can't be opened.
std::map<unsigned, std::string> load(const std::string& path, std::string* error);

struct Change {
    unsigned id;
    char kind;  // 'A' added, 'R' removed, 'C' changed (renamed)
    std::string oldName, newName;
};

// Compares two loaded listfiles. `a` is the older snapshot, `b` the newer.
// An ID present in both with the same name produces no entry (only actual
// changes are reported).
std::vector<Change> diff(const std::map<unsigned, std::string>& a, const std::map<unsigned, std::string>& b);

// Parses a plain FileDataID worklist: one decimal ID per line (e.g. the
// output of `list --unresolved-only --format text`, or any other tool's
// idea of "these FileDataIDs need attention"). Blank lines and lines that
// aren't purely digits are silently skipped, same tolerance as load() above.
// Sets *error and returns empty only if the file itself can't be opened.
std::vector<unsigned> loadIdList(const std::string& path, std::string* error);

}  // namespace listfile
