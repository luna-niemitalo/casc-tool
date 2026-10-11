# Move the build history store to another pool without copying what the game
# can reproduce: run `casc-tool snapshot <to>` first (re-extracts the live
# build from CASC, fast on SSD), then this copies only the objects <to> lacks
# (older builds, legacy path builds, stand-ins the exports had decrypted), each
# MD5-verified, plus the build records and tags, and remounts.
#
# Deletes nothing: <from> stays until you remove it yourself, after the final
# check here says every one of its objects exists in <to>.
#
# usage: nu scripts/migrate-store.nu [--dry-run]

const repo = path self ..

def main [
    --from: path = /media/luna/data/wow_storage
    --to: path = /media/luna/work/wow_storage
    --mountpoint: path = /media/luna/wow_export
    --listfile: path = ($repo | path join development community-listfile-2026-10-08.csv)
    --dry-run
] {
    let casc = $repo | path join build casc-tool
    let scratch = $to | path join tmp migrate
    mkdir $scratch

    # --- precondition: the live build was re-snapshotted into <to> ----------
    let liveLabel = open ($from | path join tags live) | str trim
    let fromRoot = $from | path join builds $liveLabel ROOT
    let toRoot = $to | path join builds $liveLabel ROOT
    if not ($toRoot | path exists) {
        error make {msg: $"($toRoot) missing -- run `casc-tool snapshot ($to) ...` first"}
    }
    if (open --raw $fromRoot | hash md5) != (open --raw $toRoot | hash md5) {
        error make {msg: $"live build ($liveLabel) has a different ROOT in ($from) and ($to)"}
    }

    # --- objects <to> lacks ---------------------------------------------------
    let fromList = $scratch | path join from.txt
    let toList = $scratch | path join to.txt
    let need = $scratch | path join need.txt
    ^sh -c $"cd '($from)' && find objects -type f | sort > '($fromList)'"
    ^sh -c $"cd '($to)' && find objects -type f | sort > '($toList)'"
    ^sh -c $"comm -23 '($fromList)' '($toList)' > '($need)'"
    let count = open $need | lines | length
    print $"objects: ($from) has (open $fromList | lines | length), ($to) has (open $toList | lines | length), copying ($count)"
    if $dry_run {
        print "dry run: nothing copied"
        return
    }

    # --- copy, then verify each copied object's content against its name -----
    ^rsync -a --files-from $need $from $to
    let bad = ^sh -c $"cd '($to)' && xargs -a '($need)' -P 8 -n 500 md5sum | awk '{n=split\($2,p,\"/\"\); if \($1 != p[n]\) print $2}'" | lines
    if ($bad | length) > 0 {
        error make {msg: $"($bad | length) copied objects don't match their names, e.g. ($bad | first)"}
    }
    print $"copied and verified ($count) objects"

    # --- build records, tags, logs (the re-snapshotted live build is kept) ---
    for dir in [builds tags logs] {
        if ($from | path join $dir | path exists) {
            ^rsync -a --ignore-existing ($from | path join $"($dir)/") ($to | path join $"($dir)/")
        }
    }

    # --- final check: nothing of <from> is missing from <to> ------------------
    ^sh -c $"cd '($to)' && find objects -type f | sort > '($toList)'"
    let stillMissing = ^sh -c $"comm -23 '($fromList)' '($toList)' | wc -l" | str trim | into int
    if $stillMissing > 0 { error make {msg: $"($stillMissing) objects of ($from) still missing in ($to)"} }
    let fromBuilds = ls ($from | path join builds) | get name | path basename | sort
    let toBuilds = ls ($to | path join builds) | get name | path basename | sort
    if ($fromBuilds | where {|b| $b not-in $toBuilds} | length) > 0 { error make {msg: "a build record didn't copy"} }
    print $"complete: every object and build of ($from) is in ($to)"

    # --- remount ---------------------------------------------------------------
    if (^findmnt -n $mountpoint | complete).exit_code == 0 {
        let r = ^fusermount3 -u $mountpoint | complete
        if $r.exit_code != 0 { ^fusermount3 -u -z $mountpoint }  # busy (a file manager): detach lazily
    }
    ^$casc mount $to $mountpoint --listfile $listfile
    print $"mounted ($to) at ($mountpoint)"
    print $"($from) is now redundant; remove it yourself when satisfied"
}
