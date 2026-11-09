# CLAUDE.md — casc-tool

Global rules apply (`~/.claude/CLAUDE.md`). Nix conventions: `.claude/rules/nix.md`.
Read `DESIGN.md` before any structural change.

## Purpose

Read WoW's local CASC storage, and keep a deduplicated per-build history of it
(the store) that can be diffed and browsed.

## Status

- **Current / Target**: `DESIGN.md` § Status (single source; not repeated here).
- Live store: `/media/luna/work/wow_storage`, mounted at `/media/luna/wow_export`.
- Anything not listed as built there does not exist yet.

## Boundaries

- The game install: `.build.info` (`storage::activeProducts`), CASC via CascLib
  (`vendor/CascLib`, git submodule), ROOT bytes (`rootmanifest::parse`).
- Listfiles (`development/community-listfile-<date>.csv`, from
  wowdev/wow-listfile releases) and TACT keys (`development/TACTKeys`, git clone
  of wowdev/TACTKeys).
- Export trees fed to `snapshot --seed-from` / `import` (content-matched by md5).
- The store's own records (`meta.json`, `*.tsv`) are interior: written by casc-tool.

## Resume

- **Last state** (2026-11-09): multi-product support (`wow` = `_retail_`,
  `wowf` = `_forever_`): snapshot records every active product, `builds` and
  `changes` added, CascLib bumped to upstream 6b889ec, listfile 2026-11-07
  downloaded. Live store's import records backfilled with `"product": "wow"`.
  First `wowf` snapshot (1.60.1.70790) done in the live store (98,794 FDIDs
  not downloaded by the `_forever_` client: see CHANGELOG 2026-11-09); log in
  `<store>/logs/snapshot-2026-11-09-wow-wowf.log`. Committed. Store mounted
  at `/media/luna/wow_export` with the 2026-11-07 listfile (`live`, `forever`, ...).
- **Next step**: after the next patch, `casc-tool snapshot <store> ...` once
  (records both products), then `changes <store> live` / `changes <store> forever`.
  DESIGN.md "Not built yet" lists the remaining commands.
- **Hazards**: `build/` was reconfigured with `cmake --fresh` (old cache pointed
  at garbage-collected nix store paths). `nix/flake.lock` has Luna's uncommitted
  change, plus `nix/flake.lock.bak`: not ours, leave them.
