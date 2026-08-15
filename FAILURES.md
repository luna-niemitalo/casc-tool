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
