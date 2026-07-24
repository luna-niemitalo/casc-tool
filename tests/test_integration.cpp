// Integration tests: run the actual compiled casc-tool binary as a
// subprocess against a real CASC storage, and check its stdout/stderr/exit
// code. Deliberately not mocked -- CascLib/BLTE/CASC's actual behavior is
// exactly what caught the two real bugs during manual testing, and a mock
// would have hidden both of them.
//
// Requires two environment variables (set by whoever runs the suite, e.g.
// the parent wow_modding project after mounting its real install):
//   CASC_TOOL_TEST_STORAGE   a real CASC storage root (contains .build.info)
//   CASC_TOOL_TEST_LISTFILE  a matching listfile.csv
// Every test below skips itself (with a logged reason, not a failure) if
// these aren't set, so the suite stays runnable without a real WoW install
// -- see tools/casc-tool/README.md's Testing section for the tradeoff.
//
// Some of these tests encode the *intended* behavior of casc-tool rather
// than its current behavior, deliberately -- see the top-level project's
// notes on the descriptive-failure-message matrix. A red test here is doing
// its job.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unistd.h>
#include <string>
#include <vector>

#include <doctest/doctest.h>

namespace {

std::optional<std::string> envVar(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::optional<std::string>(v) : std::nullopt;
}

std::optional<std::string> testStorage() { return envVar("CASC_TOOL_TEST_STORAGE"); }
std::optional<std::string> testListfile() { return envVar("CASC_TOOL_TEST_LISTFILE"); }

#define SKIP_WITHOUT_REAL_STORAGE()                                                                 \
    do {                                                                                            \
        if (!testStorage() || !testListfile()) {                                                    \
            MESSAGE("SKIPPED (no real storage available): set CASC_TOOL_TEST_STORAGE and "          \
                    "CASC_TOOL_TEST_LISTFILE to exercise this test");                                \
            return;                                                                                 \
        }                                                                                           \
    } while (0)

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

struct ProcResult {
    int exitCode;
    std::string out;
    std::string err;
};

// Runs the actual compiled casc-tool binary with the given arguments,
// capturing stdout and stderr separately.
ProcResult runCasc(const std::vector<std::string>& args) {
    std::string errFile =
        (std::filesystem::temp_directory_path() / ("casc-tool-test-stderr-" + std::to_string(::getpid()) +
                                                     "-" + std::to_string(rand())))
            .string();

    std::string cmd = shellQuote(CASC_TOOL_BINARY);
    for (const auto& a : args) cmd += " " + shellQuote(a);
    cmd += " 2>" + shellQuote(errFile);

    ProcResult result;
    FILE* pipe = popen(cmd.c_str(), "r");
    REQUIRE_MESSAGE(pipe != nullptr, "popen() itself failed -- can't run casc-tool at all");

    std::array<char, 4096> buf;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        result.out.append(buf.data(), n);
    }
    int status = pclose(pipe);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    std::ifstream errIn(errFile);
    result.err.assign(std::istreambuf_iterator<char>(errIn), std::istreambuf_iterator<char>());
    std::filesystem::remove(errFile);

    return result;
}

std::vector<std::string> withStorage(std::vector<std::string> args) {
    args.push_back("--storage");
    args.push_back(*testStorage());
    args.push_back("--listfile");
    args.push_back(*testListfile());
    return args;
}

// Pulls "scanned S entries, M matched, N shown" out of `list`'s stderr
// summary line. Fails the calling test if the line isn't found in the
// expected shape, rather than silently returning zeros.
struct ListSummary {
    unsigned long long scanned, matched, shown;
};

ListSummary parseListSummary(const std::string& stderrText) {
    ListSummary s{};
    int found = std::sscanf(stderrText.c_str(), "scanned %llu entries, %llu matched, %llu shown", &s.scanned,
                            &s.matched, &s.shown);
    if (found != 3) {
        // The summary line isn't necessarily the first line (progress
        // output precedes it); search line by line instead.
        std::istringstream lines(stderrText);
        std::string line;
        while (std::getline(lines, line)) {
            if (std::sscanf(line.c_str(), "scanned %llu entries, %llu matched, %llu shown", &s.scanned,
                            &s.matched, &s.shown) == 3) {
                return s;
            }
        }
        FAIL("couldn't find 'scanned N entries, M matched, K shown' in stderr:\n" << stderrText);
    }
    return s;
}

unsigned long long countOccurrences(const std::string& haystack, const std::string& needle) {
    unsigned long long count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

std::vector<char> readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// RAII temp directory, removed (recursively) when the test ends.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("casc-tool-test-dir-" + std::to_string(::getpid()) + "-" + std::to_string(rand()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_SUITE("integration: basic sanity") {

TEST_CASE("the real storage actually opens and reports a product") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"list", "--limit", "1"}));
    CHECK(r.exitCode == 0);
    CHECK(r.err.find("product=") != std::string::npos);
}

}  // TEST_SUITE("integration: basic sanity")

TEST_SUITE("integration: --limit actually limits") {
// This is a direct regression test for the real bug found manually: `list
// --format csv|json` used to ignore --limit entirely and dump every
// matching row (a "limit 3" test printed 3.19 million lines before this was
// caught).

TEST_CASE("--limit caps text output at exactly N rows") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"list", "character/bloodelf/female/*", "--format", "text", "--limit", "5"}));
    CHECK(r.exitCode == 0);
    // Each shown row is printed as "  [name] ..." or "  [id  ] ..."; the
    // first row has no preceding newline in stdout (the banner is on
    // stderr), so anchor on "  [" itself, not "\n  [".
    CHECK(countOccurrences(r.out, "  [") == 5);
}

TEST_CASE("--limit caps csv output at exactly N data rows") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"list", "character/bloodelf/female/*", "--format", "csv", "--limit", "5"}));
    CHECK(r.exitCode == 0);
    long dataRows = std::count(r.out.begin(), r.out.end(), '\n') - 1;  // minus the header line
    CHECK(dataRows == 5);
}

TEST_CASE("--limit caps json output at exactly N objects") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"list", "character/bloodelf/female/*", "--format", "json", "--limit", "5"}));
    CHECK(r.exitCode == 0);
    CHECK(countOccurrences(r.out, "\"fdid\"") == 5);
}

TEST_CASE("--limit 0 means unlimited: shown equals matched, not capped at the default") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"list", "character/bloodelf/female/*", "--limit", "0"}));
    CHECK(r.exitCode == 0);
    auto summary = parseListSummary(r.err);
    CHECK(summary.shown == summary.matched);
    CHECK(summary.matched > 100);  // sanity: this mask really does match more than the old default cap
}

}  // TEST_SUITE("integration: --limit actually limits")

TEST_SUITE("integration: descriptive failure messages") {
// The four distinct ways a lookup can fail, per the real testing session:
// listfile itself missing, name not in the listfile, name in the listfile
// but the file's data isn't in this local install, and a FileDataID that
// doesn't exist in CASC at all. Right now several of these collapse to the
// same generic strerror(ENOENT) text -- these tests encode that they
// SHOULDN'T, and are expected to fail until that's fixed.

TEST_CASE("listfile path itself doesn't exist") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc({"info", "character/bloodelf/female/bloodelffemale.m2", "--storage", *testStorage(),
                      "--listfile", "/definitely/does/not/exist.csv"});
    CHECK(r.exitCode != 0);
    CHECK_MESSAGE(r.err.find("listfile") != std::string::npos,
                  "expected the error to mention the *listfile* specifically as the problem, got:\n" << r.err);
}

TEST_CASE("name not present in the listfile at all") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"info", "totally/bogus/path/that/is/not/real.m2"}));
    CHECK(r.exitCode != 0);
    CHECK(r.err.find("totally/bogus/path/that/is/not/real.m2") != std::string::npos);
}

TEST_CASE("name found in listfile, but the file isn't available in this local install") {
    SKIP_WITHOUT_REAL_STORAGE();
    // FileDataID 21 is a legacy cinematic (logo_1024.avi) confirmed present
    // in the listfile but NOT shipped in a modern retail install.
    auto r = runCasc(withStorage({"info", "21"}));
    CHECK(r.exitCode != 0);
    bool mentionsUnavailability = r.err.find("locally") != std::string::npos || r.err.find("available") != std::string::npos;
    CHECK_MESSAGE(mentionsUnavailability,
                  "expected the error to say the file is known but not available locally, got:\n" << r.err);
}

TEST_CASE("FileDataID doesn't exist in CASC at all") {
    SKIP_WITHOUT_REAL_STORAGE();
    auto r = runCasc(withStorage({"info", "4000000000"}));
    CHECK(r.exitCode != 0);
    CHECK(r.err.find("4000000000") != std::string::npos);
}

// Replaces every occurrence of `subject` with a placeholder before
// comparing messages for distinctness. Without this, two messages built
// from the exact same template ("couldn't open '%s': %s") would compare as
// "different" purely because they echo back different queried subjects
// (a bogus path vs a bogus FileDataID) -- which would make the test pass
// without the messages actually explaining anything different. Comparing
// only the template/explanation text is the point of this test.
std::string withoutSubject(std::string msg, const std::string& subject) {
    size_t pos;
    while ((pos = msg.find(subject)) != std::string::npos) {
        msg.replace(pos, subject.size(), "<SUBJECT>");
    }
    return msg;
}

TEST_CASE("the three CASC-level failure reasons produce genuinely different explanations") {
    SKIP_WITHOUT_REAL_STORAGE();
    const std::string bogusPath = "totally/bogus/path/that/is/not/real.m2";
    const std::string unavailableId = "21";
    const std::string bogusId = "4000000000";

    auto notInListfile = runCasc(withStorage({"info", bogusPath}));
    auto notAvailableLocally = runCasc(withStorage({"info", unavailableId}));
    auto notInCasc = runCasc(withStorage({"info", bogusId}));

    std::string m1 = withoutSubject(notInListfile.err, bogusPath);
    std::string m2 = withoutSubject(notAvailableLocally.err, unavailableId);
    std::string m3 = withoutSubject(notInCasc.err, bogusId);

    CHECK_MESSAGE(m1 != m2,
                  "'not in listfile' and 'not available locally' use the same explanation template "
                  "once the echoed subject is normalized out -- they only look different because "
                  "they mention different inputs, not because they explain different problems");
    CHECK_MESSAGE(m1 != m3,
                  "'not in listfile' and 'no such FileDataID' use the same explanation template "
                  "once the echoed subject is normalized out");
    CHECK_MESSAGE(m2 != m3,
                  "'not available locally' and 'no such FileDataID' use the same explanation template "
                  "once the echoed subject is normalized out");
}

}  // TEST_SUITE("integration: descriptive failure messages")

TEST_SUITE("integration: extract-batch dry-run") {

TEST_CASE("dry-run's predicted count and bytes match the real run exactly") {
    SKIP_WITHOUT_REAL_STORAGE();
    const std::string mask = "character/bloodelf/female/*";

    auto dry = runCasc(withStorage({"extract-batch", "--dry-run", mask, "/unused"}));
    REQUIRE(dry.exitCode == 0);
    unsigned long long predictedFiles = 0, predictedBytes = 0;
    REQUIRE(std::sscanf(dry.out.c_str(), "would extract %llu files, %llu bytes", &predictedFiles,
                        &predictedBytes) == 2);

    TempDir outDir;
    auto real = runCasc(withStorage({"extract-batch", mask, outDir.string()}));
    REQUIRE(real.exitCode == 0);
    unsigned long long actualExtracted = 0, actualTotal = 0, actualFailed = 0;
    REQUIRE(std::sscanf(real.out.c_str(), "extracted %llu/%llu files (%llu failed)", &actualExtracted,
                        &actualTotal, &actualFailed) == 3);

    CHECK(actualFailed == 0);
    CHECK(actualTotal == predictedFiles);
    CHECK(actualExtracted == predictedFiles);

    // Cross-check against bytes actually sitting on disk, not just the
    // tool's own self-reported total.
    unsigned long long bytesOnDisk = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(outDir.string())) {
        if (entry.is_regular_file()) bytesOnDisk += entry.file_size();
    }
    CHECK(bytesOnDisk == predictedBytes);
}

}  // TEST_SUITE("integration: extract-batch dry-run")

TEST_SUITE("integration: open-by-path vs open-by-FileDataID") {
// Regression test for the other real bug found manually: info/extract by
// path failed even for files `list` had just resolved by name, because
// CascOpenFile's by-name mode only works for names already registered via
// a Find pass. Fixed by always resolving through CascFindFirstFile first.

TEST_CASE("extracting the same file by path and by FileDataID produces byte-identical output") {
    SKIP_WITHOUT_REAL_STORAGE();
    const std::string path = "character/bloodelf/female/bloodelffemale.m2";
    const std::string id = "116921";

    TempDir dir;
    std::string outByPath = dir.string() + "/by_path.m2";
    std::string outById = dir.string() + "/by_id.m2";

    auto r1 = runCasc(withStorage({"extract", path, outByPath}));
    REQUIRE_MESSAGE(r1.exitCode == 0, "extract by path failed:\n" << r1.err);
    auto r2 = runCasc(withStorage({"extract", id, outById}));
    REQUIRE_MESSAGE(r2.exitCode == 0, "extract by FileDataID failed:\n" << r2.err);

    auto bytesA = readWholeFile(outByPath);
    auto bytesB = readWholeFile(outById);
    CHECK_FALSE(bytesA.empty());
    CHECK(bytesA == bytesB);
}

}  // TEST_SUITE("integration: open-by-path vs open-by-FileDataID")
