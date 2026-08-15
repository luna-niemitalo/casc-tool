# casc-tool

A small command-line tool for browsing and extracting files out of a World
of Warcraft [CASC](https://wowdev.wiki/CASC) storage — the local,
content-addressed archive format the game client stores its data in. If
you've used `wow.export`'s file browser before, this is the same underlying
job, but as a scriptable CLI with no GUI/Chromium runtime attached.

If you're new to both WoW modding *and* the command line: every command
below is copy-pasteable as written, on Linux. You don't need to know C++ to
use this tool — only to hack on it.

This tool is standalone — it doesn't assume any other project's directory
layout, scripts, or tooling. Everything you need to get it running is
listed below or fetched from its own actual upstream source.

## What you need before any of this works

1. **A build of the tool.** See "Installing"/"Building" below.
2. **A WoW install to actually read.** This tool never downloads game data
   itself, and never writes to the one you point it at — it only opens
   files for reading. `--storage` just needs to point at a directory
   containing a `.build.info` file (that's the real WoW install root, e.g.
   wherever Battle.net installed it, or a copy of it). If you'd rather not
   point this tool at your live install directly, mount/copy it read-only
   somewhere first — how you do that is up to you and your OS; this repo
   doesn't prescribe a mechanism.
3. **A listfile.** CASC identifies files by number (`FileDataID`), not by
   name — the listfile is what maps `1234` to
   `character/bloodelf/female/bloodelffemale.m2`. Get one from its actual
   upstream source: [wowdev/wow-listfile releases](https://github.com/wowdev/wow-listfile/releases) —
   download `community-listfile.csv` and pass it as `--listfile`.
4. **(Optional) A TACT keys file, if you hit encrypted content.** Most of
   CASC is plain (BLTE-compressed, not encrypted); a small amount is
   genuinely encrypted client-side and unreadable without the matching key,
   independent of whether the bytes are present locally. Same shape as the
   listfile above — this tool never fetches or bundles keys itself, you
   provide a file: get one from its actual upstream source,
   [wowdev/TACTKeys](https://github.com/wowdev/TACTKeys) (`WoW.txt`, kept
   in sync as new keys are found the same way `wow-listfile` is), and pass
   it as `--keys`. Skip this step entirely if you never see "file is
   encrypted and the decryption key is missing" — most workflows never
   will.

## Installing (as a Nix package)

This is a real flake package, not just a compilable source tree — `nix/flake.nix`
exposes `packages.default` and `apps.default`, so it's installable/runnable
the same way any other flake-packaged CLI tool is, without needing this repo
checked out at all:

```
nix run github:luna-niemitalo/casc-tool -- --help          # try it without installing
nix profile install github:luna-niemitalo/casc-tool         # install to your user profile
```

Or as an input to another flake (e.g. a home-manager config):

```nix
{
  inputs.casc-tool.url = "github:luna-niemitalo/casc-tool";
  # ...
  home.packages = [ inputs.casc-tool.packages.${pkgs.system}.default ];
}
```

**Why this needed more than "just cmake":** CascLib is vendored as a git
submodule for local dev, but a submodule's actual file contents aren't part
of what `git ls-files` reports for the parent repo (only a gitlink entry
is) — so a plain `nix build` of the flake's own source wouldn't have seen
it. The fix was to fetch CascLib as its own separate flake input instead
(pinned via the normal flake-lock mechanism, currently
`ladislav-zezula/CascLib` commit `4d6258f`) and splice it into
`vendor/CascLib` during the build, rather than depending on the consumer
remembering `?submodules=1` on a `github:` reference to *this* repo. The
package source itself is filtered down to exactly `CMakeLists.txt` + `src/`
via `lib.fileset` — `tests/`, `build/`, and `.git` never enter the Nix store
for this build at all.

## Building (from source, for development)

From this directory (`tools/casc-tool/`), inside the project's Nix dev shell
(`direnv exec .` from the repo root, or `nix develop ./nix -c bash` first):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

That's it — no package manager, no `pip`/`npm`/etc. involved. It statically
links [CascLib](https://github.com/ladislav-zezula/CascLib) (vendored as a
git submodule under `vendor/CascLib`; if you cloned this repo fresh and that
directory looks empty, run `git submodule update --init` first). The binary
lands at `build/casc-tool`.

To run it without typing the full path every time, either add
`tools/casc-tool/build` to your `PATH`, or copy/symlink the binary
somewhere already on it (`~/.local/bin`, etc.):

```
ln -s "$(pwd)/build/casc-tool" ~/.local/bin/casc-tool
```

The rest of this document assumes you've done that and can just type
`casc-tool`. If not, substitute `./build/casc-tool` (from this directory) or
`./tools/casc-tool/build/casc-tool` (from the repo root).

## Shell completion

Bash and zsh completion scripts are checked in at `completions/casc-tool.bash`/
`.zsh` — every subcommand, every flag, `--format`/`--locale` value choices
where those are enumerable, `--storage` and `extract-batch`'s `<out-dir>`
completing directories rather than files. Source whichever matches your
shell:

```
source completions/casc-tool.bash     # bash, this session only
source completions/casc-tool.zsh      # zsh, this session only
```

or copy it into wherever your shell's completion loader already looks
(`/etc/bash_completion.d/`, a `$fpath` entry for zsh's `compinit`, etc.) for
it to load automatically in new shells.

These files are **captured output**, not hand-written — `casc-tool
--print-completion=bash` (or `=zsh`) generates them from the tool's own,
real, currently-parsed option tables (see `src/completion.cpp`'s own top
comment for the mechanism), so they can't silently drift the way a
hand-maintained completion script would the next time a flag is added or
renamed. Regenerate after any option-table change:

```
casc-tool --print-completion=bash > completions/casc-tool.bash
casc-tool --print-completion=zsh > completions/casc-tool.zsh
```

`--print-completion` itself doesn't show up in `casc-tool --help` — its only
consumers are this regeneration step and the installed script's own
callback, so it has no reason to occupy space in the help text a human
actually reads.

## Quick start

```
cd /path/to/your/wow/install    # the directory containing .build.info
cp /wherever/you/downloaded/community-listfile.csv ./listfile.csv

casc-tool list --limit 10
casc-tool extract character/bloodelf/female/bloodelffemale.m2
casc-tool extract 116921         # by path or by FileDataID -- both work everywhere
```

`--storage` defaults to `.` and `--listfile` to `listfile.csv`, both
relative to the current directory — that's why the quick start above `cd`s
into the install first. Working from somewhere else, or against multiple
installs? Pass `--storage <path> --listfile <path>` explicitly; nothing
above requires being run from any particular directory.

`casc-tool --help` and `casc-tool <command> --help` are always up to date
and slightly more terse than this document; use this README for the *why*,
`--help` for the exact flag spelling.

## Commands

Run `casc-tool --help` for the full list, or `casc-tool <command> --help`
for a command's exact options. Summary of what each is for:

- **`list [mask]`** — see what's in the storage. `mask` is a glob over
  in-storage paths (`'character/bloodelf/*'`, `'*.m2'`); default `*`
  (everything). Supports `--format text|csv|json` for piping into other
  tools, `--unresolved-only` to see FileDataIDs the listfile doesn't have a
  name for yet (the actual worklist for growing the community listfile —
  see [wow-listfile](https://github.com/wowdev/wow-listfile)), and
  `--limit` (default 100, `0` = unlimited, applies to every format).

- **`info <id-or-path>`** — metadata for one file (content/encoded key,
  size, span count, locale/content flags) without extracting it. Useful for
  checking a file is actually present before a bulk extract.

- **`extract <id-or-path> [out-file]`** — pull one file to disk. Defaults
  the output filename to the file's own basename (or CascLib's
  `FILE########.dat` convention if you only gave a bare ID).

- **`extract-batch <mask> <out-dir>`** — bulk version of `extract`: every
  match gets written to `<out-dir>/<in-game path>`, mirroring the game's own
  folder layout (entries with no known name go to `<out-dir>/_unresolved/`).
  Run with `--dry-run` first on anything you're not sure about the size of —
  it reports the file count and total bytes without writing anything.
  **`--from-list <ids-file> <out-dir>`** is an alternate form: instead of a
  glob mask, give it a plain text file of explicit FileDataIDs (one decimal
  ID per line — e.g. another tool's own "these are missing" worklist) and it
  extracts exactly those, opening the storage once regardless of list size.
  Use this over a mask whenever you already know the exact IDs you want; a
  mask re-walks/re-scans the whole root on every invocation, which doesn't
  matter for one `extract-batch` call but does if you're driving it from a
  worklist of thousands of IDs one at a time. See "What this actually
  found" below for the real-world case that motivated it.

- **`diff <listfile-a> <listfile-b>`** — compare two listfile snapshots and
  report FileDataIDs added, removed, or renamed. Doesn't touch any storage —
  pure listfile-to-listfile comparison. Useful for "what did this patch
  actually add" if you keep old listfile downloads around before replacing
  them with a fresh one.

Every storage-touching command shares the same five options:
`--storage`, `--listfile`, `--locale`, `--keys`, `--product` — run any
command's `--help` to see the full description of each; they behave
identically everywhere.

## Troubleshooting

**"couldn't open storage ... : No such file or directory"** — the path you
gave `--storage` doesn't contain a `.build.info` file, i.e. it's not
actually a WoW install root. Remember it also defaults to `.` (the current
directory) if you don't pass `--storage` at all.

**A file `list`/`info` shows exists, but `extract` fails with "No such file
or directory"** — some FileDataIDs (older cinematics in particular) simply
aren't shipped in every install; the listfile knows a name for them, but
your local copy of the game never downloaded the actual bytes. This is
normal, not a bug in this tool — there isn't a local-storage way around it
short of installing more of the game (or, later, this tool's planned
CDN/online mode — see "Design notes" below).

**"file is encrypted and the decryption key is missing"** — some CASC
content is genuinely encrypted client-side; without the matching key it's
permanently unreadable, not just unnamed or undownloaded — a different
problem from either of those, and this is the only message that means it.
Pass a keys file via `--keys <file>`: one `KeyName KeyValue` hex pair per
line, get it from its actual upstream source,
[wowdev/TACTKeys](https://github.com/wowdev/TACTKeys) (`WoW.txt`), same
"download it yourself, this tool never fetches it" contract as
`--listfile`. [wow.tools's tactkeys API](https://wow.tools/api.php?type=tactkeys)
mirrors the same underlying data if you'd rather query it than keep a file
around. Not every encrypted FileDataID has a publicly known key yet —
`--keys` closes the ones that do; the rest stay unreadable until someone
finds that specific key, which is a real, external, moving target this
tool has no way to shortcut.

For an encrypted FileDataID whose key `--keys` does happen to cover, the
shape looks like this: `extract`/`info` fails with the message above when
run without `--keys`, then succeeds once run again with `--keys keys.txt`
pointed at a file that contains that ID's real key — no other change.
Same before/after `--listfile` already gives you for a *name*, just for a
*byte* instead of a string.

**A file variant seems to be missing depending on region/language** — pass
`--locale <name>` (see `casc-tool list --help` for the list of names); the
default `all` should normally already cover this, but voice-over audio in
particular is locale-gated.

## Testing

Uses [doctest](https://github.com/doctest/doctest) (via nixpkgs — no
vendoring needed for this one). Two tiers, split by whether they need a real
CASC storage:

- **Pure-logic tests** (`tests/test_cli.cpp`, `test_format.cpp`,
  `test_storage.cpp`, `test_listfile.cpp`, `test_completion.cpp`) —
  argument parsing, csv/json escaping, locale-name parsing, error-message
  mapping, listfile/ID-list loading/diffing, and (for `test_completion.cpp`)
  that every real flag name for every command actually shows up in the
  generated shell-completion output. No external dependencies, always run,
  exhaustive by design (every documented behavior of these modules has a
  test, not just the happy path).
- **Integration tests** (`tests/test_integration.cpp`) — run the actual
  compiled `casc-tool` binary as a subprocess against a real CASC storage
  and check its stdout/stderr/exit code. Deliberately not mocked: CascLib's
  real behavior is exactly what caught two real bugs during manual testing
  (see below), and a mock would have hidden both.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/casc-tool-tests                    # pure-logic tests only; integration tests skip themselves
```

To actually run the integration tests, point two environment variables at a
real WoW install and a listfile (see "What you need before any of this
works" above for where to get one):

```
CASC_TOOL_TEST_STORAGE=/path/to/your/wow/install \
CASC_TOOL_TEST_LISTFILE=/path/to/listfile.csv \
  ./build/casc-tool-tests
```

Without those set, every integration test logs `SKIPPED (no real storage
available)` and doctest counts it as passed rather than failed — a
deliberate tradeoff, given a synthetic CASC fixture would be real
engineering effort in its own right (CascLib's own test suite doesn't ship
one either — it just runs against real, large game installs): the suite
stays runnable anywhere, but a fully green run without the env vars set only
means "the pure logic is fine," not "verified end to end." Set the env vars
to get the real signal.

**What the integration suite actually found, testing against the real, live
122GB install** (this is the point of writing tests this way instead of
only unit-testing in isolation):

- `list --format csv|json` silently ignoring `--limit` (now fixed, and
  regression-tested).
- `info`/`extract` by path failing for files `list` had just resolved by
  name (now fixed — `storage::openFile` always resolves through
  `CascFindFirstFile` first — and regression-tested with a byte-identical
  by-path-vs-by-ID comparison).
- Three message-clarity gaps that used to collapse into the same generic
  `strerror(ENOENT)` text — a nonexistent `--listfile` path, a
  known-but-not-locally-available FileDataID/path, and a path/FileDataID
  that plain doesn't exist in CASC at all, all indistinguishable from each
  other. Now fixed (each gets its own message; see `storage::openFile`/
  `storage::checkListFileExists` in `src/storage.cpp`) and regression-tested,
  including a check that the three explanations are genuinely different
  templates, not just different because they echo different input.

A later, more thorough pass against the same real install (see
`FAILURES.md` for the full write-up) found several more gaps, now
regression-tested the same way (red until fixed, per the philosophy above):
an invalid `--product` codename gets blamed on the storage path instead of
being reported as a bad product; a directory passed as `--listfile` is
silently accepted instead of erroring, with every file coming back
"unresolved"; non-numeric/negative `--limit` either crashes into a raw
`std::stol` message or silently no-ops after a full scan; and
`--unresolved-only`'s "worklist" output includes CASC entries that have no
FileDataID at all (`CASC_INVALID_ID`), which aren't nameable files and
can't be opened by `info`/`extract` either. One item from that pass,
`extract-batch`'s output-path construction never independently
sanitizing `..`-style path-traversal components (it currently relies
entirely on an internal, unstated CascLib invariant instead), is a known,
accepted gap — see `FAILURES.md` for why it's being left as-is.

**What `extract-batch --from-list` actually found, run for real against a
live install (2026-08-16):** a sibling project (`husk`, a WoW model
converter) had flagged 18,747 distinct FileDataIDs as a "genuine CASC
re-extraction gap" — textures its own corpus scan couldn't find locally
under any name, after already filtering out the common false positive of a
real extraction keeping a file under its own name elsewhere in the tree
(see that project's own findings for the full methodology). Feeding that
exact worklist to `extract-batch --from-list` against the live install, one
storage open, ~13 seconds:

```
would extract 18742/18747 files, 3287782818 bytes
```

**18,742 of 18,747 (99.97%) were sitting in CASC the whole time** — not
actually missing from the game data, just never re-extracted into that
corpus. The real (non-dry-run) run wrote all 3,287,782,818 bytes to their
real listfile-resolved paths with zero failures on the recoverable set.
Only 5 were genuinely absent: 4 with no manifest entry in this build at
all, and 1 (`creature/treasuregoblinpet/d3textures.blp`, FileDataID
940334) with a real entry but data that was simply never downloaded to
this install — the same "known but not shipped" case this README's
Troubleshooting section already documents, just discovered at scale
instead of one file at a time. This is the real-world case `--from-list`
was built for: checking (or filling) a large, externally-produced worklist
of specific FileDataIDs is a fundamentally different access pattern than
"walk everything matching a glob," and paying for one storage open instead
of thousands is the whole point.

## Design notes (for anyone extending this)

- **One small verb per real workflow, not one command with a hundred
  flags.** The instruction that shaped this was, roughly, "comprehensive
  but not `yt-dlp`" — every flag above exists because a concrete step in an
  actual workflow needed it, not because it seemed like it might be useful
  someday. Machine-readable output (`--format csv|json`) is how this stays
  composable without every command needing to grow every other command's
  features.
- **Shell completion is generated, not hand-written, and reads real option
  metadata instead of a second flag list.** `src/registry.cpp` is the one
  place that lists every subcommand (name, `run`/`help`, and — the part
  that matters here — the exact `cli::OptionSpec` vector that command's own
  argument parser and `--help` already use); `main.cpp`'s dispatch and
  `src/completion.cpp`'s `--print-completion=<bash|zsh>` generator both read
  that same table, so a completion script can't silently drift from what
  the tool actually accepts the way a hand-maintained one could. Ported
  from the identical design in the sibling `husk` project's `DESIGN.md`
  ("Shell completion generation") — that version walks a throwaway CLI11
  `App` tree via CLI11's introspection API since husk uses CLI11 and no C++
  arg-parsing library (CLI11 included) generates completions on its own;
  casc-tool doesn't need that step at all, because `cli::OptionSpec` *is*
  already the live, structured metadata a generator needs — this tool's own
  minimal, dependency-free parser (see `cli.hpp`'s top comment) turns out to
  be sufficient introspection surface by construction. What's *not* derived
  from that metadata (which flags/positionals are directories vs. files,
  `--format`'s enum values) is a small hand-kept table in
  `completion.cpp` — see its own top comment for exactly what and why.
- **`storage::openFile` always resolves through `CascFindFirstFile`, even
  for a bare path, even though CascLib also has a direct by-name open
  mode.** This isn't stylistic — it's a real, empirically-found constraint:
  WoW's CASC root stores name *hashes*, not names, so `CascOpenFile`'s
  by-name mode only succeeds for names CascLib has already learned via a
  `Find` pass on that same storage handle. Opening a file by path that
  `list` had *just* resolved, without going through `Find` first, failed
  with `ERROR_FILE_NOT_FOUND` — confirmed with `strace` before landing on
  the current approach. See the comment on `storage::openFile` in
  `src/storage.hpp`.
- **If you're pointing `--storage` at a read-only mount/copy of your install
  rather than the live one directly**, be aware CascLib opens `.build.info`
  (and a couple of other storage-detection files) with `O_RDWR` as an
  internal probe, even though it never actually persists anything through
  that handle — found empirically via `strace`. A strictly read-only mount
  (e.g. `mount -o ro` or `bindfs -o ro`) will make that open fail with
  `EROFS` and `CascOpenStorage` will report "not found," which is confusing
  the first time you hit it. A copy-on-write overlay (real data as the
  lowerdir, which overlayfs never writes to, with a disposable upper layer
  absorbing CascLib's harmless probe-writes) sidesteps this while still
  fully protecting the real install — that's the mechanism to reach for if
  you want both protection and compatibility.
- **Planned, not yet built:** online/CDN (TACT) access, to pull straight
  from Blizzard's CDN without a full local install. A genuinely different
  code path from local-storage reading — worth keeping separate rather than
  conflating the two just because they'd share a CLI.
- **Not planned, by decision, not just deferred:** writing back into CASC
  or injecting modified assets. Not a goal of this tool, and not even a
  well-documented path upstream in CascLib itself.
- No plugin system, no config file, no TUI. If a future need doesn't fit
  "one verb, a few flags, text/csv/json out," that's a sign to reconsider
  the need, not to bolt on a bigger parser.

## Disclaimer

This tool was co-coded by AI. It's verified by a massively autistic
developer — every claim in this README (the empirical findings, the test
results, the failure modes) was checked against the real thing, not taken
on faith.
