# Ingest the older export trees into the store, deduplicating as it goes.
# Builds pinned 2026-10-11 by matching each tree's ROOT hash against the
# `root =` of public build configs (wago.tools build list -> Blizzard CDN):
#   wow_export_old/ROOT                 b5cdb435...  = WOW-68887patch12.0.7_Retail
#   wow_export_superseded/pre-69933/ROOT e33cac7f... = WOW-69404patch12.1.0_Retail
#     (wow_export's ROOT before its 2026-10-10 incremental update to 69933;
#      the leftovers in wow_storage/legacy came from that same tree)
#
# Each ROOT import moves files whose content its ROOT lists into objects/ and
# drops files whose content is already stored *and* listed. A path import
# then records whatever no ROOT accounts for, at the path it had, as an
# unknown-version build. Every move and drop is hash-verified (cmd_import.cpp).
# Safe to re-run: moved files are gone from the trees, the rest is re-checked.
#
# usage: nu scripts/ingest-old-exports.nu

const repo = path self ..

def main [
    --store: path = /media/luna/data/wow_storage
    --old: path = /media/luna/data/wow_export_old
    --superseded: path = /media/luna/data/wow_export_superseded/pre-69933
] {
    let casc = $repo | path join build casc-tool
    let legacy = $store | path join legacy
    let move = [--move --drop-duplicates]

    print "== 12.1.0.69404: the 127 pre-update copies, then whatever in legacy/ that build lists"
    ^$casc import $store $superseded --label 12.1.0.69404 --root ($superseded | path join ROOT) ...$move
    ^$casc import $store $legacy --label 12.1.0.69404 --root ($superseded | path join ROOT) ...$move

    print "== legacy-wow_export: what remains of legacy/, at its own paths"
    ^$casc import $store $legacy --label legacy-wow_export --tag legacy ...$move

    print "== 12.0.7.68887: wow_export_old against its own ROOT"
    ^$casc import $store $old --label 12.0.7.68887 --root ($old | path join ROOT) --tag old ...$move

    print "== legacy-wow_export_old: what remains of wow_export_old, at its own paths"
    ^$casc import $store $old --label legacy-wow_export_old --tag legacy-old ...$move

    for tree in [$legacy $old $superseded] {
        ^find $tree -mindepth 1 -type d -empty -delete
        let left = ^find $tree -type f | lines | length
        print $"($tree): ($left) files left"
    }
}
