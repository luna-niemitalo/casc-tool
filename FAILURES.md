# casc-tool: failure findings

Produced by exercising the tool against a real, live WoW retail install
(`product=wow build=68887 total_files=3190909`, ~2.2M-line
`community-listfile.csv` downloaded fresh from
[wowdev/wow-listfile](https://github.com/wowdev/wow-listfile/releases)),
mounted read-only via an overlayfs copy-on-write layer exactly as README.md's
own "Design notes" section recommends. Build: `cmake --build build` inside
the project's Nix dev shell, no modifications to source. Every item below
was reproduced against the real binary, not inferred from reading code alone
(code reading was used to explain *why*, after observing the behavior).

Each item is a single, independent failure/gap. Severity is my own call:
**major** (breaks a documented workflow or produces actively wrong output),
**bug** (wrong/inconsistent behavior, narrower blast radius), **latent**
(a real defect, not yet triggered by today's data), or **coverage gap** (no
test exercises this at all, pass or fail).

Every item below except #1 (docs-only, nothing to assert in code) and #12
(won't-fix, see that item) now has an automated test in
`tests/test_format.cpp` or `tests/test_integration.cpp` — each item names
its test(s) and whether the last run was **red** (fails today, demonstrates
the bug; will flip green once fixed) or **green** (already correct,
coverage gap closed).

---

## 1. [major, fixed] README's own "Testing" section is stale — it documents 3 bugs as still-open that are actually already fixed and green

**Status: fixed.** README.md's "Testing" section and the corresponding
comments in `tests/test_integration.cpp` (top-of-file and the
"descriptive failure messages" suite) have been rewritten to describe the
three message-clarity gaps as resolved/regression-tested, matching what
the suite actually shows, and now point at this file for the current set
of known-red, intended-behavior tests instead.

`README.md`'s "Testing" section (and matching comments in
`tests/test_integration.cpp`) claims:

> **Three still-open gaps**, currently failing on purpose ... a red test
> here is doing its job, not a mistake

listing (1) a missing `--listfile` path not being reported as a listfile
problem, (2) a known-but-not-locally-available file producing a generic
message, (3) "not in listfile" vs "no such FileDataID" sharing one message
template.

Running the actual integration suite against the real install shows all
three passing:

```
CASC_TOOL_TEST_STORAGE=<overlay merged dir> CASC_TOOL_TEST_LISTFILE=<listfile> \
  ./build/casc-tool-tests --test-suite="integration: descriptive failure messages"
...
[doctest] test cases:  5 |  5 passed | 0 failed | 57 skipped
[doctest] assertions: 18 | 18 passed | 0 failed |
```

`storage::openFile`/`storage::checkListFileExists` in `src/storage.cpp`
already implement distinct messages for all three cases ("listfile not
found: ...", "... is known but its data isn't available in this local
install ...", "no file with FileDataID ... exists in this storage"). The
code was fixed; the README/test-file commentary describing them as
open/expected-to-fail was never updated to match. Anyone reading the
README to understand the tool's current state gets actively wrong
information about what's broken.

**Fix direction:** delete the "three still-open gaps" section (or move it
to a changelog), and drop the "expected to fail" framing from the
integration test file comments now that they're regression tests, not
red-by-design tests.

---

## 2. [major] `list --unresolved-only` — the documented "listfile worklist" — is 98.5% non-file noise

README describes `--unresolved-only`:

> the actual worklist for growing the community listfile (see
> wowdev/wow-listfile)

Run against the real install with today's freshest upstream listfile:

```
casc-tool list --unresolved-only --format csv --limit 0
...
scanned 3190906 entries, 1315266 matched, 1315266 shown
```

Of those 1,315,266 "unresolved" rows, **1,295,274 (98.5%)** have
`fdid=4294967295` (`CASC_INVALID_ID`, i.e. no FileDataID at all) and a name
field that's a raw CKey/EKey hex string, not a path:

```
4294967295,35769,0,00000fa77dd8de28f15c18596ae2de44
4294967295,17964,0,0000106a7fa01d487bc335670b124f9f
```

Only the remaining ~19,993 rows are genuine "FileDataID known, name
missing" entries (the actual `FILE########.dat` placeholders README
describes). The CKey/EKey-only entries are CASC storage components with no
FileDataID and no in-game path at all (patch/component data, not assets) —
they are not nameable the way a real unresolved FileDataID is, and don't
belong in a "listfile contribution worklist" in the first place. Piping
`--unresolved-only --format csv` straight into a listfile-contribution
workflow, as the README's own example (`> unnamed.csv`) suggests, produces
a file that's 98.5% garbage for that purpose.

**Fix direction:** filter out (or put in a clearly separate bucket from)
entries with `dwFileDataId == CASC_INVALID_ID` before calling them part of
the "unresolved FileDataID" worklist.

**Test:** `tests/test_integration.cpp`, suite "integration: --unresolved-only
worklist purity" → `"--unresolved-only never reports CASC_INVALID_ID as if
it were a real FileDataID"`. **Red** — currently fails (real run found
`4294967295,` rows in the CSV output).

---

## 3. [major] Those same `fdid=4294967295` rows are displayed as if they were real, usable FileDataIDs, but nothing in the tool can act on them

Following directly from #2: `list`'s text/csv/json output prints
`CASC_INVALID_ID` (`4294967295`) in the exact same `fdid` column/field as a
real FileDataID, with no marker that it means "no ID." A user (or a script
consuming the CSV) has no way to tell it apart from a legitimate ID other
than recognizing the specific sentinel value `4294967295` by eye.

Confirmed it's genuinely unusable:

```
$ casc-tool info 4294967295 --storage ... --listfile ...
error: no file with FileDataID 4294967295 exists in this storage
```

Worse, `extract-batch` doesn't special-case this either — `outPathFor` in
`src/cmd_extract_batch.cpp` will happily route such an entry to
`<out-dir>/_unresolved/FILE_FFFFFFFF.dat` and then try
`CascOpenFile(..., CASC_FILE_DATA_ID(0xFFFFFFFF), ...)`, which fails. Since
these entries are ~40% of *all* storage entries (1,295,274 of 3,190,906),
running `extract-batch '*' out/` (or any broad mask that isn't scoped away
from them) would print roughly 1.3 million `warning: ...` lines to stderr
and inflate the "failed" counter by over a million, drowning out any
genuinely actionable extraction failures in the same run.

**Fix direction:** skip/report `CASC_INVALID_ID` entries distinctly (e.g. a
one-line stderr summary count) instead of treating each one as a per-file
failure, and give them a distinguishable marker in `list` output.

**Test:** shares its regression test with #2 (same root cause, same
`CASC_INVALID_ID` rows) — see #2's "worklist purity" test, currently red.
The `extract-batch`-floods-stderr consequence described above isn't
separately covered: reproducing it for real means running
`extract-batch '*'` over the whole ~3.19M-entry storage (the ~1.3M-warning
flood only shows up at that scale), which is too slow to run as a routine
test. Flagging that as a known coverage gap on top of the finding itself,
not adding a test for it.

---

## 4. [bug] An invalid `--product` codename produces a misleading "check your storage path" error

The storage path is 100% valid (works fine without `--product`); only the
codename is wrong:

```
$ casc-tool list --storage <valid path> --listfile <valid listfile> --product totally_bogus_product --limit 1
error: couldn't open storage '<valid path>': No such file or directory
       (expecting a directory containing .build.info -- see README.md §2.1)
```

The hint text is flatly wrong here — the directory does contain
`.build.info`, and does open fine for every other invocation. The root
cause is `storage::open()` in `src/storage.cpp` unconditionally printing
the same "expecting a directory containing .build.info" hint on *any*
`CascOpenStorageEx` failure, regardless of what actually caused it. This is
the exact same "generic message masks the real cause" failure mode the
project explicitly calls out and tests for elsewhere (see item #1) — just
not extended to storage-open failures at all, and with zero test coverage
(see item #9).

**Fix direction:** inspect `GetCascError()` after a failed
`CascOpenStorageEx` and only show the `.build.info` hint when the error is
actually consistent with a bad/missing storage path; give `--product`
mismatches their own message.

**Test:** `tests/test_integration.cpp`, suite "integration: --product error
message accuracy" → `"an invalid --product codename isn't blamed on the
storage path"`. **Red** — currently fails (the `.build.info` hint still
shows up). This test also closes #9's coverage gap.

---

## 5. [bug] A directory passed as `--listfile` is silently accepted and produces total, unexplained name-resolution failure

`storage::checkListFileExists` (`src/storage.cpp`) only calls
`std::filesystem::exists(path)`, which is true for directories too:

```
$ casc-tool list --storage <valid path> --listfile <valid path (a directory)> --limit 1
...
opened storage: product=wow build=68887 total_files=3190909
scanned 3190906 entries, 3190906 matched, 1 shown
  [id  ] fdid=21 size=4609024 FILE00000015.dat
```

Every single file in the storage silently reports as unresolved
(`[id  ]`, `FILE########.dat`) — CascLib fails to read the "listfile"
(because it's a directory) but nothing surfaces that failure. Compare to a
genuinely missing path, which *is* caught cleanly:

```
$ casc-tool list --storage <valid path> --listfile /does/not/exist.csv --limit 1
error: listfile not found: '/does/not/exist.csv'
```

A user who fat-fingers `--listfile` to point at a directory (e.g. their
WoW install root instead of the CSV inside it) gets a fully "successful"
run with zero actionable signal that anything is wrong — indistinguishable
from a huge/fresh install with a genuinely low name-resolution rate.

**Fix direction:** `checkListFileExists` should also require
`std::filesystem::is_regular_file(path)`, with a message naming the
listfile path as the problem, same as the missing-file case.

**Test:** `tests/test_integration.cpp`, suite "integration: --listfile must
be a file, not a directory" → `"passing a directory as --listfile is
reported as a listfile problem, not silently accepted"`. **Red** —
currently fails (exit code stays 0, stderr never mentions "listfile").

---

## 6. [bug] Non-numeric `--limit` crashes into a raw, unhelpful C++ exception message

```
$ casc-tool list --limit banana --storage ... --listfile ...
error: stol
(exit code 2)
```

`cmd_list.cpp` does `std::stol(args.optionOr("--limit", "100"))` with no
validation and no catch — the exception's `.what()` ("stol", from
libstdc++) reaches `main`'s generic `catch (const std::exception&)` and
gets printed verbatim. Every other user-input mistake in this tool (bad
`--format`, bad `--locale`, wrong positional count) gets a clear,
actionable message; this one doesn't.

**Fix direction:** validate the `--limit` string (or catch
`std::invalid_argument`/`std::out_of_range` around the `stol` call) and
raise a `cli::ArgError` with a message like "‑‑limit must be a whole number
≥ 0".

**Test:** `tests/test_integration.cpp`, suite "integration: --limit input
validation" → `"a non-numeric --limit doesn't crash into a raw std::stol
exception message"`. **Red** — currently fails (`error: stol` still comes
through). Needs no real storage; runs unconditionally.

---

## 7. [bug] Negative `--limit` is silently accepted, does a full expensive scan, and shows nothing

```
$ casc-tool list --limit -5 --storage ... --listfile ...
...
scanned 3190906 entries, 3190906 matched, 0 shown
  ... (3190906 more; rerun with --limit 0 to see all)
```

`--limit -5` isn't `0` (the documented "unlimited" sentinel), so
`willPrint = limit == 0 || shown < limit` is false for every row (`shown`
starts at 0, which is never `< -5`) — the tool does the *entire* storage
scan (same cost as `--limit 0`) and then reports zero rows shown, with a
"rerun with --limit 0" hint that has nothing to do with the actual
problem. No error, no warning that `-5` isn't a sane value.

**Fix direction:** same validation as #6 — reject negative values (other
than treating them as equivalent to some documented meaning) with a clear
`ArgError` instead of a silent, expensive no-op.

**Test:** same suite as #6 → `"a negative --limit doesn't silently show
zero rows after a full scan"`. **Red** — currently fails (1243 real matches
on the test mask, 0 shown).

---

## 8. [latent, untested] `format::jsonEscape` doesn't escape all JSON-mandated control characters

`src/format.hpp`'s `jsonEscape` only special-cases `"`, `\`, `\n`, `\r`,
`\t`. Per RFC 8259, every control character `U+0000`–`U+001F` must be
escaped in a JSON string; this function passes the rest (e.g. `\b` 0x08,
`\f` 0x0c, vertical tab 0x0b, and everything else in that range) through
completely unescaped. That would produce invalid JSON for `list --format
json` / `diff --format json` if such a byte ever showed up in a listfile
name.

Not currently triggered — I checked the live, current 2.2M-line
`community-listfile.csv` and confirmed zero bytes in that range appear in
any name — so this hasn't broken anyone yet. But it's unvalidated foreign
data (the listfile is exactly the kind of external input the project's own
conventions say should be validated at the boundary) with no guard and no
test, one unusual upstream listfile entry away from emitting broken JSON
that a downstream `jq`/parser would reject outright.

**Fix direction:** escape the full `U+0000`–`U+001F` range (e.g. `\u00XX`
for anything without a named short escape), and add a test case for at
least one such byte.

**Test:** `tests/test_format.cpp`, suite `format::jsonEscape` → `"C0 control
characters without a short escape become \u00XX (RFC 8259)"` and `"every C0
control character except \n \r \t is escaped somehow, never emitted raw"`.
**Red** — both fail (4/4 and 28/28 assertions respectively; every
untested control byte still passes through raw).

---

## 9. [coverage gap, partially closed] `--product` had zero test coverage, anywhere

Neither `tests/test_cli.cpp`, `tests/test_storage.cpp`, nor
`tests/test_integration.cpp` exercised `--product` at all — not the happy
path (a real multi-flavor install), not the failure path (this is how #4
went unnoticed). It's one of the five options every storage command
documents and shares (`storage::commonOptionSpecs()`).

**Test:** the failure path is now covered by #4's test (same test, see
above) — currently red, since #4 itself is still open. Still no happy-path
test (a valid, non-default `--product` codename against a real multi-flavor
install) — this install only has the one `wow` flavor available, so that
side of the gap remains open regardless of #4's fix.

---

## 10. [coverage gap, closed] `--keys` (malformed or missing file) had zero test coverage

`storage::open()` has an explicit "couldn't load --keys file" warn-and-continue
branch (`src/storage.cpp`), reachable both for a missing path and for a
file that isn't in the expected `KeyName KeyValue` format. Confirmed both
by hand:

```
$ casc-tool list --keys /does/not/exist/keys.txt ...
warning: couldn't load --keys file '/does/not/exist/keys.txt': No such file or directory

$ casc-tool list --keys <file containing "garbage not a key file"> ...
warning: couldn't load --keys file '...': bad/unrecognized file format
```

Both behave reasonably (warn, don't abort) — but no test anywhere asserted
this is the intended behavior, so a future change to either path could
silently start hard-failing (or silently stop warning) with nothing to
catch it.

**Test:** `tests/test_integration.cpp`, suite "integration: --keys
warn-and-continue behavior" → `"a missing --keys file warns by name and
doesn't abort the command"` and `"a malformed --keys file warns by name and
doesn't abort the command"`. **Green** — both pass; coverage gap closed,
current behavior pinned down as a regression test.

---

## 11. [coverage gap, closed] `info --format json` and `diff --format json`/`csv` were never asserted against, by any test

The integration suite's `--format` coverage was entirely about `list`
(the `--limit` regression tests). `cmd_info.cpp`'s JSON branch and
`cmd_diff.cpp`'s CSV/JSON branches are documented, user-facing,
machine-readable output formats that had not a single test (unit or
integration) checking their structure, field names, or escaping. Given
`format::csvEscape`/`jsonEscape` are shared, generic bugs would be caught
incidentally through `list`, but format-specific issues (e.g. field
ordering, a typo'd key name, a missing comma) in `info`/`diff`'s
hand-written `printf` JSON would not have been.

**Test:** `tests/test_integration.cpp`, suite "integration: info --format
json structure" → `"info --format json emits one well-formed JSON object
with the documented fields"`, and suite "integration: diff --format
json/csv structure" → `"diff --format csv emits a header plus one row per
change"` and `"diff --format json emits a JSON array with one object per
change"`. **Green** — all three pass; coverage gap closed. (The `diff`
tests need no real storage, unlike everything else in this file — `diff`
is pure listfile-to-listfile comparison.)

---

## 12. [hardening gap, will not fix] `extract-batch`'s output path is never sanitized against path-traversal components — it relies entirely on an unstated CascLib invariant

**Status: will not fix**, at least for now. Reasoning below stands as
originally written; the call is that this is a real defense-in-depth gap
worth documenting, not a demonstrated live exploit worth spending effort
on — the CascLib-side hash check makes actually constructing a malicious
listfile entry impractical (would require a Jenkins-hash collision against
a real root entry's stored name hash), and no path to trigger it has been
found. Revisit if CascLib's `RootFormatWoW_v1` path (the one where
`pFileNode->FileNameHash == 0` and the check is skipped entirely, per the
`CascRootFile_WoW.cpp` reference below) turns out to be reachable in
practice, or if `extract`/`extract-batch` ever start trusting listfile
names from a source less constrained than "CascLib's own root file".

`outPathFor()` in `src/cmd_extract_batch.cpp` builds
`<out-dir>/<listfile-derived name>` with no check for `..` components or an
absolute-looking name. In practice this is currently mitigated **inside
CascLib**, not casc-tool: `CascRootFile_WoW.cpp`'s `Search()` only accepts
a listfile-supplied name for a FileDataID if that name's computed hash
matches the hash already stored in the root file for that ID (a crafted
`--listfile` entry claiming a bogus name for a real ID gets silently
rejected, confirmed by reading `src/CascRootFile_WoW.cpp:595-601` in the
vendored copy) — *except* that check is skipped entirely when the root
entry has no stored name-hash (`pFileNode->FileNameHash == 0`), in which
case whatever name the listfile supplies is trusted outright.

This isn't a demonstrated live exploit (I did not find a way to construct
a colliding name in the time available, and typical WoW root entries do
carry a name hash), but it means casc-tool's own path-construction code has
*no* boundary validation of its own for data it explicitly treats as
foreign/untrusted elsewhere (the project's own conventions call for
validating foreign input at the boundary, one field → one check). Right
now the only thing standing between a crafted listfile and a write outside
`<out-dir>` is an internal, unstated, undocumented CascLib behavior that
could change between versions.

**Fix direction:** normalize/reject `..` components and absolute paths in
`outPathFor()` before joining with `out-dir`, independent of whatever
CascLib does upstream.

**Test:** none added. `outPathFor()` is a static (anonymous-namespace)
function in `src/cmd_extract_batch.cpp`, not exposed via a header, so it
isn't reachable from a unit test without first extracting it into a
testable function — and a black-box CLI repro would need an actual
Jenkins-hash collision against a real root entry, which isn't practical to
construct. Both are open questions for whoever picks this back up, not
something to fake a test around.
