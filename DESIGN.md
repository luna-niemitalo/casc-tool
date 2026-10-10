# casc-tool — build history store (design, not yet built)

Status: **target**. Nothing in this document exists in code yet; README.md
describes the current tool. This section becomes "current" piece by piece as
the commands below land.

## Problem

Every patch currently costs a full re-export (`/media/luna/data/wow_export`,
240 GB) and keeping an older build means a second full copy
(`wow_export_old`). Measured 2026-10-10 with a throwaway ROOT differ:

| from → to | FDIDs | changed | added | removed |
|---|---|---|---|---|
| `wow_export_old` → `wow_export` | 1.93 M | 19,807 | 35,955 | 1,034 |
| `wow_export` → installed 12.1.0.69933 | 1.93 M | 161 | 0 | 0 |

~97% of every copy is a duplicate. `tank/data` has compression and dedup
off, so ZFS saves nothing either.

Wanted: see exactly which files changed / stayed the same between any two
builds, and read any file at any stored build, paying disk only for content
that actually changed.

## Core idea

CASC already content-addresses everything: a file's **CKey is the MD5 of its
decoded bytes** (verified: `altarofstorms_000.wmo` CKey `810d37f5…` == md5 of
the exported copy). So:

- **Objects**: each distinct decoded file is stored once, named by its CKey.
- **Builds**: a build is just its ROOT manifest — the FDID → CKey mapping.
- **Names** are a third, independent axis (listfile snapshots), because
  names improve over time without the build changing.

Diffing two builds is a merge-join of two sorted manifests; reading a file at
build B is `manifest[B][fdid] → objects/<ckey>`.

## Store layout

```
<store>/
  objects/81/0d/810d37f5ce1af1523c2b6b106af1ca98   decoded bytes, name = CKey
  builds/<label>/                                   label e.g. 12.1.0.69933
    ROOT            raw root manifest bytes (the authoritative record)
    manifest.tsv    derived from ROOT: fdid  locale  content_flags  ckey   (sorted)
    missing.tsv     fdid  ckey  reason      rows whose object is not in the store
    meta.json       version, build key, cdn key, source, ingest date
  listfiles/<md5>.csv                              listfile snapshots by content hash
  views/<label>/…                                  optional browsable tree, disposable
```

Everything is plain files: `ls`, `grep`, `md5sum`, `join` work on the store
without casc-tool. Rough size per build: ROOT ~67 MB, manifest.tsv ~200 MB
uncompressed (3.2 M rows incl. locale variants).

## Invariants

1. **`objects/<k>` exists ⇒ md5(contents) == k.** Objects are written to a
   temp name, hashed, and renamed into place only on match. Zero-filled
   partial-encryption output (README, "partial-encryption fallback") never
   becomes an object — it goes to `missing.tsv` with reason `partial`.
2. **Objects are immutable and never deleted implicitly.** The store is
   append-only; removal only via an explicit `prune` (drop builds, then
   delete objects no remaining manifest references).
3. **A build's manifest derives from that build's ROOT bytes only**, through
   one MFST parser used by both ingestion paths. Names never enter a manifest.
4. **Views are derived and disposable.** Rebuildable from manifest + objects
   at any time; never the only copy of anything.
5. **casc-tool writes only under `--store`** (and view/output dirs it is
   given). It never writes into a game install or into an existing export.

## Ingestion

Local CASC storage only ever holds the *current* build, so history exists
only if each build is snapshotted while it is installed. Two paths in:

- **`snapshot`** — from the live install. Read ROOT via CascLib, parse it,
  and for every CKey not already in `objects/`, open by CKey
  (`CASC_OPEN_BY_CKEY`) and store it. A hotfix build costs only its delta
  (the 161 files above); the first snapshot costs one full extract.
  Unreadable (missing TACT key) → `missing.tsv` reason `encrypted`.
  Re-running with newer `--keys` retries missing rows whose CKey is still
  present in the installed build.
- **`import-export <dir>`** — one-time migration of an existing export
  (wow.export/CascView-style tree that contains its build's `ROOT`).
  Path-agnostic: hash **every** file under `<dir>` and keep those whose md5
  is a CKey in `<dir>/ROOT`. This ignores how the exporter named or placed
  files (`_unresolved/`, old listfile names), and invariant 1 makes a wrong
  match impossible. ROOT CKeys with no matching file → `missing.tsv` reason
  `not-in-export`. Label must be given explicitly (`--label`):
  `buildinfo.json` in both existing exports is known stale.

## Exploration commands

Same shape as the existing verbs (one verb per workflow, `--format
text|csv|json`, globs resolved through `--listfile`, default = newest stored
listfile):

| command | answers |
|---|---|
| `builds` | which builds are stored, row/object/missing counts, bytes unique to each |
| `changes <A> <B> [mask]` | added / removed / changed / unchanged per FDID (`--status` filter; unchanged hidden by default, counted always) |
| `history <id-or-path>` | the CKey of one file across every stored build, transitions marked |
| `get <id-or-path> --build <B> [out]` | one file at one build, from the store |
| `mount <mountpoint>` | browse every stored build by path (see Browsing) |
| `view <B> <dir> [mask]` | materialize a named tree of build B as real files |

`get`, `mount` and `view` pick the enUS row where a FDID has locale
variants (decided 2026-10-10); `--locale` overrides.

`changes --format csv` includes both object paths per row, so content-level
tools (husk, `cmp`, image diffing) consume the change list directly; casc-tool
itself does not interpret file formats.

## Browsing

Objects are named by hash, but people and tools want listfile paths. Paths
are not stable: the listfile keeps evolving (names added, renamed,
corrected), so "the path of FDID X" depends on *which* listfile. The fix is
to make that dependency explicit rather than hide it:

**A named tree is a pure function of (build, listfile snapshot).** Pin both
and the tree is deterministic; a newer listfile is just a new tree over the
same objects, at zero storage cost. Every tree also carries
`_by_fdid/<fdid>`, which is stable forever regardless of listfile, and
`_unresolved/<fdid>` for FDIDs the pinned listfile can't name.

Two ways to get a tree, same layout:

- **`mount` (primary): read-only FUSE filesystem.** Layout:
  ```
  <mnt>/live -> builds/12.1.0.69933          one tag per product: live (wow),
  <mnt>/ptr  -> builds/…                     ptr (wowt), beta (wow_beta) --
                                              never "latest", which would be ptr
  <mnt>/builds/<label>/<listfile path>       e.g. world/wmo/…/x.wmo
  <mnt>/builds/<label>/_by_fdid/<fdid>
  <mnt>/builds/<label>/_unresolved/<fdid>
  <mnt>/builds/<label>/.listfile             which snapshot named this tree
  ```
  Names resolve at mount time against `--listfile` (default: newest stored
  snapshot), so a new listfile is just a remount, with nothing to
  regenerate. Every program on the machine (file managers, Blender, husk,
  `grep -r`) sees ordinary read-only files, which is the "dedicated browser"
  without a dedicated UI. Read-only by construction, so the hardlink hazard
  below cannot occur. Needs libfuse3 in the flake (approved 2026-10-10);
  mounting itself is unprivileged via NixOS's setuid
  `/run/wrappers/bin/fusermount3`, already present. Visible to all of
  Luna's processes, readable only by her unless `allow_other`.
- **`view` (fallback): materialized tree on disk** for contexts where FUSE
  isn't available or a tool insists on real files. Links objects instead of
  copying: reflink (`cp --reflink`, ZFS ≥ 2.2 block cloning — already active
  on `tank/data`, observed this session) when available, else hardlink with
  objects kept mode 0444. **Hazard (hardlink fallback):** a tool that writes
  into a view file in place corrupts that object for every build. 0444 +
  reflink-first is the mitigation; `builds --verify` re-hashes objects to
  detect it. Views record their listfile in `.listfile` too, and are
  regenerated, never patched, when the listfile moves.

`/media/luna/data/wow_export` maps onto this directly: it becomes
`<mnt>/live` (or a `view` of the live build), and consumers like husk's
corpus scans point there instead of at a hand-maintained export.

## Rejected alternatives

- **Re-export in place + ZFS snapshots.** Cheapest to set up, block-level
  sharing works, but still a full re-export per patch, `zfs diff` is
  path-level (listfile renames look like churn) and per-file history across
  snapshots is a scripting exercise. Doesn't give "which FDIDs changed".
- **ZFS dedup.** Large RAM cost, still writes every byte, no change listing.
- **restic/borg/git-annex.** Chunk-dedup works, but the repo is opaque (not
  browsable, not greppable) and knows nothing about FDIDs or CKeys — we'd
  rebuild the manifest layer anyway on top of a second hash scheme. git
  itself does not handle 240 GB of binaries.
- **SQLite index.** Not needed at 2–3 M rows per build: sorted TSV +
  merge-join is fast and inspectable. Revisit only if `history` across many
  builds gets slow (it would be a derived cache, never the source of truth).
- **Dedicated browser UI (GUI/TUI).** Would work, but only inside itself:
  every other tool would still need real paths. FUSE gives the same
  "pick a build, browse by path" to everything at once, and README already
  rules out a TUI for this tool.
- **Hash-named store browsed through symlink farms.** Many tools
  resolve symlinks and then show or record the hash path, so name context
  leaks away. FUSE and reflink views keep real names at the point of use.
- **CascLib enumeration (`CascFindFirstFile`) as the manifest source.**
  Would avoid an MFST parser, but `import-export` has no CascLib storage to
  enumerate — it needs the parser regardless, and one parser for both paths
  keeps manifests comparable (invariant 3).

## Open questions (Luna)

- **Store location.** Next to the existing exports on `tank/data`. Creating
  a dataset is a system mutation, so it's yours to run. Suggested:
  `zfs create -o compression=zstd -o recordsize=1M tank/data/wow_store` —
  DB2/M2/WMO compress well, BLP less so; objects stay raw files so views and
  external tools read them directly.
- **After migration**, whether `wow_export` / `wow_export_old` get removed —
  your call and your action; the store reports exactly what each import
  could not account for before that question even comes up.
- **Snapshot trigger.** Manual, or a check that `.build.info`'s Version
  differs from the newest stored build (the only way to never miss a build,
  since the install discards old ones).
