# casc-tool: failure findings

This is a **punch list**, not an archive: it holds only what's still
outstanding. An item is removed the moment it's fixed/closed, not marked
"[fixed]" and left in place — see `CHANGELOG.md` for the full resolved
history (every item that used to live here, moved there verbatim once
resolved, original numbering kept for anyone tracing an old reference).
Numbering here restarts as items are removed; don't rely on a number
outliving a fix.

Produced by exercising the tool against a real, live WoW retail install,
mounted read-only via an overlayfs copy-on-write layer exactly as README.md's
own "Design notes" section recommends. Every item below was reproduced
against the real binary, not inferred from reading code alone.

Severity is my own call: **major** (breaks a documented workflow or
produces actively wrong output), **bug** (wrong/inconsistent behavior,
narrower blast radius), **latent** (a real defect, not yet triggered by
today's data), **coverage gap** (no test exercises this at all, pass or
fail), or **hardening gap** (a real defense-in-depth weakness, not a
demonstrated live exploit).

---

## 1. [coverage gap, open] `--product`'s happy path has no test

The failure path (an invalid codename) is fixed and regression-tested (see
`CHANGELOG.md`'s item 4). The happy path — a valid, non-default `--product`
codename actually selecting the right flavor on a real multi-flavor
install — has no test at all, and can't be closed from this machine:
checked this install's own `.build.info` directly, and it lists exactly
one product line (`wow`, no `wowt`/PTR or other flavor alongside it), so
there's nothing here to select *between*. Not fixable in code either way —
needs a real multi-flavor install to write the test against, full stop.

## 2. [bug, open] invalid `--product` is blamed on the storage path when `--storage` is the `Data/` dir

`--storage` pointed at the install's `Data/` directory (the form in
`casc-tool-command.txt`, and which CascLib accepts for a valid product)
plus a bogus `--product` reports "couldn't open storage … expecting a
directory containing .build.info" — the wrong culprit, since `.build.info`
lives in `Data/`'s parent. Same call with `--storage` at the install root
gives the correct `--product` message. Found 2026-10-10: the integration
test "an invalid --product codename isn't blamed on the storage path"
fails with `CASC_TOOL_TEST_STORAGE=.../World of Warcraft/Data/` and passes
with `.../World of Warcraft`. Likely the blame heuristic in
`storage::open` checks `.build.info` only in `--storage` itself, not its
parent. Not yet investigated in code.
