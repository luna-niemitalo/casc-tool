#include <cstdio>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include "listfile.hpp"

namespace {

// Writes `content` to a fresh temp file and returns its path. The file is
// deleted when the returned guard goes out of scope, so a test can't leak
// files into /tmp even if an assertion fails partway through.
class TempListfile {
public:
    explicit TempListfile(const std::string& content) {
        path_ = std::filesystem::temp_directory_path() /
                ("casc-tool-test-" + std::to_string(counter_++) + ".csv");
        std::ofstream out(path_);
        out << content;
    }
    ~TempListfile() { std::filesystem::remove(path_); }

    TempListfile(const TempListfile&) = delete;
    TempListfile& operator=(const TempListfile&) = delete;

    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

}  // namespace

TEST_SUITE("listfile::load") {

TEST_CASE("parses well-formed FileDataId;path lines") {
    TempListfile file(
        "1;interface/cinematics/logo_800.avi\n"
        "21;interface/cinematics/logo_1024.avi\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    CHECK(error.empty());
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(1) == "interface/cinematics/logo_800.avi");
    CHECK(entries.at(21) == "interface/cinematics/logo_1024.avi");
}

TEST_CASE("strips a trailing carriage return (CRLF line endings)") {
    TempListfile file("1;interface/cinematics/logo_800.avi\r\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    REQUIRE(entries.size() == 1);
    CHECK(entries.at(1) == "interface/cinematics/logo_800.avi");
}

TEST_CASE("skips blank lines without producing a bogus entry") {
    TempListfile file("1;a.blp\n\n2;b.blp\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    CHECK(entries.size() == 2);
}

TEST_CASE("skips a line with no ';' separator rather than failing the whole parse") {
    TempListfile file("1;a.blp\nnotanentry\n2;b.blp\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    CHECK(error.empty());
    CHECK(entries.size() == 2);
    CHECK(entries.at(1) == "a.blp");
    CHECK(entries.at(2) == "b.blp");
}

TEST_CASE("skips a line with a non-numeric ID rather than failing the whole parse") {
    TempListfile file("1;a.blp\nabc;bad.blp\n2;b.blp\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    CHECK(error.empty());
    CHECK(entries.size() == 2);
}

TEST_CASE("a nonexistent path sets *error and returns an empty map") {
    std::string error;
    auto entries = listfile::load("/does/not/exist/at/all.csv", &error);
    CHECK_FALSE(error.empty());
    CHECK(entries.empty());
}

TEST_CASE("later duplicate IDs in the same file win (last one loaded wins)") {
    TempListfile file("1;first.blp\n1;second.blp\n");
    std::string error;
    auto entries = listfile::load(file.string(), &error);
    REQUIRE(entries.size() == 1);
    CHECK(entries.at(1) == "second.blp");
}

}  // TEST_SUITE("listfile::load")

TEST_SUITE("listfile::diff") {

TEST_CASE("an ID present in b but not a is reported as added") {
    std::map<unsigned, std::string> a = {{1, "a.blp"}};
    std::map<unsigned, std::string> b = {{1, "a.blp"}, {2, "b.blp"}};
    auto changes = listfile::diff(a, b);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].id == 2);
    CHECK(changes[0].kind == 'A');
    CHECK(changes[0].newName == "b.blp");
}

TEST_CASE("an ID present in a but not b is reported as removed") {
    std::map<unsigned, std::string> a = {{1, "a.blp"}, {2, "b.blp"}};
    std::map<unsigned, std::string> b = {{1, "a.blp"}};
    auto changes = listfile::diff(a, b);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].id == 2);
    CHECK(changes[0].kind == 'R');
    CHECK(changes[0].oldName == "b.blp");
}

TEST_CASE("an ID present in both with a different name is reported as changed, with both names") {
    std::map<unsigned, std::string> a = {{21, "interface/cinematics/logo_1024.avi"}};
    std::map<unsigned, std::string> b = {{21, "sound/music/citymusic/darnassus/intro.mp3"}};
    auto changes = listfile::diff(a, b);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].id == 21);
    CHECK(changes[0].kind == 'C');
    CHECK(changes[0].oldName == "interface/cinematics/logo_1024.avi");
    CHECK(changes[0].newName == "sound/music/citymusic/darnassus/intro.mp3");
}

TEST_CASE("an ID present in both with the same name produces no change entry") {
    std::map<unsigned, std::string> a = {{1, "a.blp"}};
    std::map<unsigned, std::string> b = {{1, "a.blp"}};
    CHECK(listfile::diff(a, b).empty());
}

TEST_CASE("two empty listfiles diff to no changes") {
    CHECK(listfile::diff({}, {}).empty());
}

TEST_CASE("a mix of added, removed, and renamed all show up together") {
    std::map<unsigned, std::string> a = {{1, "keep.blp"}, {2, "removed.blp"}, {3, "old_name.blp"}};
    std::map<unsigned, std::string> b = {{1, "keep.blp"}, {3, "new_name.blp"}, {4, "added.blp"}};
    auto changes = listfile::diff(a, b);
    REQUIRE(changes.size() == 3);

    bool sawAdded = false, sawRemoved = false, sawRenamed = false;
    for (const auto& c : changes) {
        if (c.kind == 'A' && c.id == 4) sawAdded = true;
        if (c.kind == 'R' && c.id == 2) sawRemoved = true;
        if (c.kind == 'C' && c.id == 3) sawRenamed = true;
    }
    CHECK(sawAdded);
    CHECK(sawRemoved);
    CHECK(sawRenamed);
}

}  // TEST_SUITE("listfile::diff")

TEST_SUITE("listfile::loadIdList") {

TEST_CASE("parses one decimal FileDataID per line") {
    TempListfile file("118017\n118034\n118346\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    CHECK(error.empty());
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 118017);
    CHECK(ids[1] == 118034);
    CHECK(ids[2] == 118346);
}

TEST_CASE("strips a trailing carriage return (CRLF line endings)") {
    TempListfile file("118017\r\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 118017);
}

TEST_CASE("skips blank lines without producing a bogus entry") {
    TempListfile file("1\n\n2\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    CHECK(ids.size() == 2);
}

TEST_CASE("skips a line with trailing junk after the digits, rather than truncating it") {
    // "5abc" must not silently become the ID 5 -- same std::stoul trap
    // --limit had (see FAILURES.md's history for that one).
    TempListfile file("1\n5abc\n2\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    CHECK(error.empty());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
}

TEST_CASE("skips a non-numeric line rather than failing the whole parse") {
    TempListfile file("1\nnotanid\n2\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    CHECK(error.empty());
    CHECK(ids.size() == 2);
}

TEST_CASE("a nonexistent path sets *error and returns empty") {
    std::string error;
    auto ids = listfile::loadIdList("/does/not/exist/at/all.txt", &error);
    CHECK_FALSE(error.empty());
    CHECK(ids.empty());
}

TEST_CASE("duplicate IDs in the file are preserved, not deduplicated") {
    // Unlike load()'s map (last-write-wins), this is a plain worklist --
    // the caller (extract-batch --from-list) should see exactly what was
    // in the file, including accidental duplicates, rather than have them
    // silently vanish.
    TempListfile file("1\n1\n2\n");
    std::string error;
    auto ids = listfile::loadIdList(file.string(), &error);
    REQUIRE(ids.size() == 3);
}

}  // TEST_SUITE("listfile::loadIdList")
