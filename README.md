# casc-tool

A small command-line tool for browsing and extracting files out of a World
of Warcraft [CASC](https://wowdev.wiki/CASC) storage — the local,
content-addressed archive format the game client stores its data in. If
you've used `wow.export`'s file browser before, this is the same underlying
job, but as a scriptable CLI with no GUI/Chromium runtime attached.

If you're new to both WoW modding *and* the command line: every command
below is copy-pasteable as written, from the repository root
(`wow_modding/`), on Linux. You don't need to know C++ to use this tool —
only to hack on it.

## What you need before any of this works

1. **A build of the tool.** See "Building" below.
2. **A WoW install to actually read.** This tool never downloads game data
   itself — see the top-level [README.md §3](../../README.md#3-whats-needed-from-the-internet-from-where-and-how-to-update)
   for why, and [§2.1](../../README.md#21-external_data--protected-access-to-the-real-game-install)
   for how it gets made available here as `external_data/`.
3. **A listfile.** CASC identifies files by number (`FileDataID`), not by
   name — the listfile is what maps `1234` to
   `character/bloodelf/female/bloodelffemale.m2`. One's already vendored at
   `m2mod/mappings/listfile.csv`; keep it fresh with
   `nu ../../scripts/update-listfile.nu`.

## Installing (as a Nix package)

This is a real flake package, not just a compilable source tree — `nix/flake.nix`
exposes `packages.default` and `apps.default`, so it's installable/runnable
the same way any other flake-packaged CLI tool is, without needing this repo
checked out at all:

```
nix run github:<you>/casc-tool -- --help          # try it without installing
nix profile install github:<you>/casc-tool         # install to your user profile
```

Or as an input to another flake (e.g. a home-manager config):

```nix
{
  inputs.casc-tool.url = "github:<you>/casc-tool";
  # ...
  home.packages = [ inputs.casc-tool.packages.${pkgs.system}.default ];
}
```

(Replace `<you>/casc-tool` with wherever this ends up pushed — see the note
about network writes in the parent project if you're doing that from
within an assistant session; pushing is something only you do.)

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

## Quick start

```
# 1. Make the real game install available, read-only, at ./external_data
#    (run once per session/reboot; see the top-level README for what this
#    actually does and why)
nu scripts/external-data.nu mount        # from the repo root

# 2. See what's in it
casc-tool list --limit 10

# 3. Pull one file out, by path or by FileDataID -- both work everywhere
casc-tool extract character/bloodelf/female/bloodelffemale.m2
casc-tool extract 116921
```

Every command that touches a storage defaults to `--storage external_data
--listfile m2mod/mappings/listfile.csv` — this project's usual layout — so
none of the examples above need those flags spelled out, as long as you run
from the repository root. Run from elsewhere, or against a different
install, and just add `--storage <path> --listfile <path>`.

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

- **`diff <listfile-a> <listfile-b>`** — compare two listfile snapshots and
  report FileDataIDs added, removed, or renamed. Doesn't touch any storage —
  pure listfile-to-listfile comparison. Pairs naturally with
  `scripts/update-listfile.nu`, which leaves the previous listfile behind as
  `listfile.csv.old_<timestamp>` specifically so you can diff against it
  and see what a patch actually changed.

Every storage-touching command shares the same five options:
`--storage`, `--listfile`, `--locale`, `--keys`, `--product` — run any
command's `--help` to see the full description of each; they behave
identically everywhere.

## Troubleshooting

**"couldn't open storage ... : No such file or directory"** — the path you
gave `--storage` doesn't contain a `.build.info` file, i.e. it's not
actually a WoW install root. If you're using the project's usual layout,
make sure you ran `nu scripts/external-data.nu mount` first
(`nu scripts/external-data.nu status` tells you if it's currently mounted).

**A file `list`/`info` shows exists, but `extract` fails with "No such file
or directory"** — some FileDataIDs (older cinematics in particular) simply
aren't shipped in every install; the listfile knows a name for them, but
your local copy of the game never downloaded the actual bytes. This is
normal, not a bug in this tool — there isn't a local-storage way around it
short of installing more of the game (or, later, this tool's planned
CDN/online mode — see the top-level README's roadmap).

**"file is encrypted and the decryption key is missing"** — some CASC
content is genuinely encrypted client-side; without the matching key it's
permanently unreadable, not just unnamed. Pass a keys file (same format
as [wow.tools's tactkeys API](https://wow.tools/api.php?type=tactkeys): one
`KeyName KeyValue` hex pair per line) via `--keys <file>`.

**A file variant seems to be missing depending on region/language** — pass
`--locale <name>` (see `casc-tool list --help` for the list of names); the
default `all` should normally already cover this, but voice-over audio in
particular is locale-gated.

## Testing

Uses [doctest](https://github.com/doctest/doctest) (via nixpkgs — no
vendoring needed for this one). Two tiers, split by whether they need a real
CASC storage:

- **Pure-logic tests** (`tests/test_cli.cpp`, `test_format.cpp`,
  `test_storage.cpp`, `test_listfile.cpp`) — argument parsing, csv/json
  escaping, locale-name parsing, error-message mapping, listfile
  loading/diffing. No external dependencies, always run, exhaustive by
  design (every documented behavior of these modules has a test, not just
  the happy path).
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
real storage and a matching listfile (from the parent `wow_modding` project,
after `nu scripts/external-data.nu mount`, that's its `external_data/` and
`m2mod/mappings/listfile.csv`):

```
CASC_TOOL_TEST_STORAGE=/path/to/external_data \
CASC_TOOL_TEST_LISTFILE=/path/to/listfile.csv \
  ./build/casc-tool-tests
```

Without those set, every integration test logs `SKIPPED (no real storage
available)` and doctest counts it as passed rather than failed — a
deliberate tradeoff (see the top-level project's notes on why a synthetic
CASC fixture wasn't built): the suite stays runnable anywhere, but a fully
green run without the env vars set only means "the pure logic is fine," not
"verified end to end." Set the env vars to get the real signal.

**What the integration suite actually found, testing against the real, live
122GB install** (this is the point of writing tests this way instead of
only unit-testing in isolation):

- `list --format csv|json` silently ignoring `--limit` (now fixed, and
  regression-tested).
- `info`/`extract` by path failing for files `list` had just resolved by
  name (now fixed — `storage::openFile` always resolves through
  `CascFindFirstFile` first — and regression-tested with a byte-identical
  by-path-vs-by-ID comparison).
- **Three still-open gaps**, currently failing on purpose (encoding the
  *intended* behavior, not the current one — per the project's testing
  philosophy, a red test here is doing its job, not a mistake):
  1. A nonexistent `--listfile` path isn't reported as a listfile problem
     specifically — it's indistinguishable from the target file itself not
     existing.
  2. A FileDataID/path that resolves in the listfile but has no locally
     available data (e.g. an uninstalled legacy cinematic) produces the
     same generic "No such file or directory" as everything else, instead
     of saying it's known-but-not-installed.
  3. A path that isn't in the listfile at all and a FileDataID that doesn't
     exist in CASC at all currently produce the *literal same* message
     template (`couldn't open '<X>': No such file or directory`) — verified
     by normalizing the echoed subject out of both messages before
     comparing, specifically to rule out "they only look different because
     they mention different inputs" as a false positive.

## Design notes (for anyone extending this)

- **One small verb per real workflow, not one command with a hundred
  flags.** The instruction that shaped this was, roughly, "comprehensive
  but not `yt-dlp`" — every flag above exists because a concrete step in an
  actual workflow needed it, not because it seemed like it might be useful
  someday. Machine-readable output (`--format csv|json`) is how this stays
  composable without every command needing to grow every other command's
  features.
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
- **Why an overlay mount and not a read-only bind mount** for
  `external_data/` — also found empirically (CascLib opens `.build.info`
  `O_RDWR` as an internal probe even though it never persists anything) —
  is documented in the top-level README, not repeated here.
- No plugin system, no config file, no TUI. If a future need doesn't fit
  "one verb, a few flags, text/csv/json out," that's a sign to reconsider
  the need, not to bolt on a bigger parser.
