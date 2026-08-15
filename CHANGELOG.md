# casc-tool: changelog

Resolved entries move here out of `FAILURES.md` once fixed/closed, so that
file stays a punch list (what's still outstanding) instead of an
ever-growing archive. This file is the archive — historical record only,
nothing here is actionable. Newest first.

---

## 2026-08-16 — `extract-batch` output paths now sanitize traversal components, closing the deferred hardening gap

Former `FAILURES.md` item 12/2 ("`extract-batch`'s output path is never
independently sanitized against path-traversal components") is fixed, not
just deferred. New `storage::sanitizeRelativePath` (`src/storage.hpp`/
`.cpp`) converts `\` to `/`, then drops every `.`/`..` component and any
leading `/` outright before a listfile-derived name is joined with
`--out-dir` — used by both `outPathFor()` (the original mask-based path)
and `outPathForId()` (the newer `--from-list` path, same file). A name
that sanitizes away to nothing (e.g. it was only `..` components) falls
back to the same `_unresolved/FILE########.dat` convention already used
for a genuinely nameless entry, rather than erroring the whole run over
one hostile-looking name. This no longer relies solely on CascLib's own
internal root-file name-hash check (still real, still a real mitigation,
just no longer the *only* one). Tests: `tests/test_storage.cpp`, suite
"storage::sanitizeRelativePath" (nine cases — ordinary path, backslash
conversion, leading/middle `..`, absolute path, all-`..` input, bare `.`,
collapsing `//` runs, empty input).

## 2026-08-16 — `extract-batch --from-list`

New bulk-extraction mode: `extract-batch --from-list <ids-file> <out-dir>`
takes a plain text file of explicit FileDataIDs (one decimal ID per line)
instead of a glob mask, opening the storage exactly once regardless of list
size — the mask-based path re-walks the whole root per invocation, fine for
a handful of calls but not for a worklist of thousands of IDs checked one
`extract` at a time. Motivated by a real cross-project finding: see
README.md's "What `extract-batch --from-list` actually found" for the
18,742/18,747-recovered result that prompted this. New code: `src/
cmd_extract_batch.cpp`'s `runExtractFromList`, `listfile::loadIdList`
(`src/listfile.hpp`/`.cpp`). Tests: `tests/test_listfile.cpp` (pure-logic
ID-list parsing), `tests/test_integration.cpp` suite "integration:
extract-batch --from-list" (real storage, gated the same as the rest of the
suite).

A validation-order bug surfaced by the new integration tests themselves,
fixed same session: `--from-list`'s own existence/parse was checked *after*
`--listfile`, so a broken `--from-list` path with the (still-default,
unrelated) `listfile.csv` also missing reported the wrong flag as the
problem. `--from-list` is now checked first, since it's the flag whoever's
using this mode actually just typed.

## 2026-08-16 — Shell completion generation

`casc-tool --print-completion=<bash|zsh>` generates `completions/
casc-tool.bash`/`.zsh` from the real, live `cli::OptionSpec` tables every
command's own argument parser and `--help` already use (`src/registry.cpp`
holds the one shared subcommand table; `src/completion.cpp` walks it) —
ported from the sibling `husk` project's identical `--print-completion`
design (see husk's `DESIGN.md`, "Shell completion generation"). New: `src/
registry.hpp`/`.cpp`, `src/completion.hpp`/`.cpp`, `completions/
casc-tool.bash`/`.zsh` (checked-in captured output), `tests/
test_completion.cpp` (asserts every real flag name for every command
appears in both generated scripts — the actual anti-drift guarantee, not
just the claim of one).

---

## 2026-08-15/16 — Hardening pass (see git history for exact commits)

The following were found by exercising the tool against a real, live WoW
retail install and are now fixed, tested, and green. Moved here from
`FAILURES.md` verbatim (original numbering kept for anyone tracing an old
reference) once resolved, per that file's own "punch list, not an archive"
convention.

### 1. [major, fixed] README's own "Testing" section was stale — documented 3 bugs as still-open that were already fixed and green

README.md's "Testing" section and matching comments in
`tests/test_integration.cpp` claimed three message-clarity gaps were still
open, red-by-design. Running the suite showed all three passing —
`storage::openFile`/`storage::checkListFileExists` already implemented
distinct messages for all three cases. Fixed by rewriting the README
section and dropping the "expected to fail" framing from the test file
comments.

### 2. [major, fixed] `list --unresolved-only` — the documented "listfile worklist" — was 98.5% non-file noise

Of 1,315,266 "unresolved" rows against a real install, 1,295,274 (98.5%)
had `fdid=4294967295` (`CASC_INVALID_ID`, i.e. no FileDataID at all) and a
CKey/EKey hex string instead of a path — CASC storage components, not
game assets, not nameable the way a real unresolved FileDataID is. Fixed:
`cmd_list.cpp` now tracks these in a separate `skippedNoId` counter and
excludes them from `--unresolved-only`'s matched/shown rows, surfacing the
count in the stderr summary instead of silently dropping it. Test:
`tests/test_integration.cpp`, "integration: --unresolved-only worklist
purity".

### 3. [major, fixed] Those same `fdid=4294967295` rows were displayed as if they were real, usable FileDataIDs, and `extract-batch` choked on them at scale

`list` printed `CASC_INVALID_ID` (`4294967295`) in the same `fdid` field as
a real ID with no marker; `extract-batch` would attempt `CascOpenFile` on
each one and, since these are ~40% of all storage entries, flood stderr
with over a million warnings on a broad mask. Fixed: `list` now shows
`fdid=-` (text), empty (csv), or `null` (json) for these instead of the raw
sentinel, in every format, not just `--unresolved-only`; `extract-batch`
skips them outright with one summary line instead of a per-entry warning.
Shares its regression test with #2 (same root cause).

### 4. [bug, fixed] An invalid `--product` codename produced a misleading "check your storage path" error

`storage::open()` printed the same ".build.info" hint on any
`CascOpenStorageEx` failure regardless of cause. Fixed: now checks whether
`<storage-path>/.build.info` actually exists first — if it does and
`--product` was given, a distinct "the storage path itself looks fine --
check --product '...'" hint prints instead. Test: "integration: --product
error message accuracy" — this test also closed the failure-path half of a
separate coverage-gap item (`--product` previously had zero test coverage
anywhere); the happy-path half of that gap is still genuinely open, see
`FAILURES.md`.

### 5. [bug, fixed] A directory passed as `--listfile` was silently accepted, producing total unexplained name-resolution failure

`storage::checkListFileExists` only checked `std::filesystem::exists`,
true for directories too — every file silently reported as unresolved with
zero signal anything was wrong. Fixed: now also requires
`is_regular_file`, with its own "isn't a regular file (looks like a
directory?)" message. Test: "integration: --listfile must be a file, not a
directory".

### 6. [bug, fixed] Non-numeric `--limit` crashed into a raw, unhelpful C++ exception message

`cmd_list.cpp` did `std::stol(...)` with no validation/catch; the raw
`stol` exception text reached the user. Fixed: parsed inside try/catch,
also checking the whole string was consumed (`"5abc"` no longer silently
truncates to `5`). Test: "integration: --limit input validation".

### 7. [bug, fixed] Negative `--limit` was silently accepted, did a full expensive scan, and showed nothing

`--limit -5` isn't the `0` "unlimited" sentinel, so the print condition was
never true — a full, expensive scan ran and reported zero rows shown with
an unrelated hint. Fixed: negative values now rejected outright with a
clear message, same validation pass as #6.

### 8. [latent, fixed] `format::jsonEscape` didn't escape all JSON-mandated control characters

Only special-cased `"`, `\`, `\n`, `\r`, `\t` — every other C0 control
character (`\b`, `\f`, vertical tab, ...) passed through unescaped, which
would produce invalid JSON if one ever showed up in a listfile name. Not
triggered by the real 2.2M-line listfile checked at the time, but
unvalidated foreign data with no guard. Fixed: any byte below `0x20`
without a named short escape now becomes `\u00XX`. Tests: `tests/
test_format.cpp`.

### 10. [coverage gap, closed] `--keys` (malformed or missing file) had zero test coverage

`storage::open()`'s warn-and-continue behavior for a missing/malformed
`--keys` file was correct but unasserted by any test. Closed: `tests/
test_integration.cpp`, "integration: --keys warn-and-continue behavior".

### 11. [coverage gap, closed] `info --format json` and `diff --format json`/`csv` were never asserted against, by any test

Format coverage was entirely about `list`. Closed: three new tests across
"integration: info --format json structure" and "integration: diff
--format json/csv structure".
