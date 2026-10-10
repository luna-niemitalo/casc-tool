# Convert the wow_export tree into the build history store, in place, and
# mount the live build where the export used to be read from.
#
#   /media/luna/data/wow_export  --rename-->  /media/luna/data/wow_storage
#   wow_storage/<old tree>       --rename-->  wow_storage/legacy/<old tree>
#   casc-tool snapshot --seed-from legacy --seed-move
#       every legacy file whose content is a current-build CKey is *moved*
#       (rename, no copy) into objects/; everything else stays in legacy/
#   casc-tool mount wow_storage /media/luna/wow_export
#
# Nothing is deleted except directories left empty by the moves. legacy/ is
# yours to review afterwards (stale versions, exporter metadata, files not in
# this build). Safe to re-run after an interruption: it resumes.
#
# usage: nu scripts/convert-export-to-store.nu [--dry-run]

const repo = path self ..

def main [
    --export: path = /media/luna/data/wow_export      # existing export tree (renamed away)
    --store: path = /media/luna/data/wow_storage      # store root it becomes
    --mountpoint: path = /media/luna/wow_export       # where the live build gets mounted
    --game: path = "/media/luna/games/World of Warcraft/Data/"
    --listfile: path = ($repo | path join development community-listfile-2026-10-08.csv)
    --keys: path = ($repo | path join development TACTKeys WoW.txt)
    --mask: string                                     # rehearsal only: limit the snapshot to a subtree
    --dry-run                                          # print the plan and checks, change nothing
] {
    let casc = $repo | path join build casc-tool
    let legacy = $store | path join legacy

    print $"export:     ($export)\nstore:      ($store)\nmountpoint: ($mountpoint)\ngame:       ($game)\nlistfile:   ($listfile)\nkeys:       ($keys)\ncasc-tool:  ($casc)\n"

    # --- preflight: every check before anything moves -----------------------
    for f in [$casc $listfile $keys] {
        if not ($f | path exists) { error make {msg: $"missing: ($f)"} }
    }
    if (^findmnt -n $mountpoint | complete).exit_code == 0 {
        error make {msg: $"($mountpoint) is already a mountpoint -- fusermount3 -u it first"}
    }
    if ($mountpoint | path exists) and (ls -a $mountpoint | length) > 0 {
        error make {msg: $"($mountpoint) exists and is not empty -- it must be an empty directory"}
    }

    let resuming = ($legacy | path exists) and not ($export | path exists)
    if not $resuming {
        if not ($export | path exists) { error make {msg: $"($export) does not exist"} }
        if ($store | path exists) { error make {msg: $"($store) already exists -- refusing to merge into it"} }
        # The whole point is rename, never copy: both must be one filesystem.
        let exportFs = ^findmnt -n -o SOURCE -T $export | str trim
        let storeFs = ^findmnt -n -o SOURCE -T ($store | path dirname) | str trim
        if $exportFs != $storeFs {
            error make {msg: $"($export) is on ($exportFs) but ($store) would be on ($storeFs): rename would copy"}
        }
        print $"plan: rename ($export) -> ($store), then move its contents into ($legacy)"
    } else {
        print $"resuming: ($legacy) already exists, skipping the renames"
    }
    print $"plan: snapshot the installed build, moving matching files out of ($legacy)"
    print $"plan: mount ($store) at ($mountpoint)\n"
    if $dry_run {
        print "dry run: nothing changed"
        return
    }

    # --- 1. rename the export into place as the store ------------------------
    if not $resuming {
        ^mv --no-target-directory $export $store
        mkdir $legacy
        ls -a $store | where ($it.name | path basename) != legacy | each {|e|
            ^mv --no-target-directory $e.name ($legacy | path join ($e.name | path basename))
        } | ignore
        print $"moved the old tree into ($legacy)"
    }

    # --- 2. snapshot, adopting verified files from legacy/ -------------------
    # Hashes every legacy file once (~240 GB); expect hours. Re-running resumes.
    let maskArgs = if $mask == null { [] } else { [--mask $mask] }
    ^$casc snapshot $store --storage $game --listfile $listfile --keys $keys --seed-from $legacy --seed-move ...$maskArgs

    # --- 3. tidy: only directories the moves left empty ----------------------
    ^find $legacy -mindepth 1 -type d -empty -delete
    let left = ^find $legacy -type f | lines | length
    let leftSize = ^du -sh $legacy | split row "\t" | first
    print $"legacy/ still holds ($left) files \(($leftSize)\): stale versions, exporter metadata, files not in this build"

    # --- 4. mount the live build ---------------------------------------------
    mkdir $mountpoint
    ^$casc mount $store $mountpoint --listfile $listfile
    print $"\nmounted: ($mountpoint)/live -> current build"
    print $"unmount: fusermount3 -u ($mountpoint)"
    print $"missing: ($mountpoint)/live/.missing \(encrypted / not available locally, with reasons\)"
}
