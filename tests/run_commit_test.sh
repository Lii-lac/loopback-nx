#!/bin/bash
# End-to-end test of the write path, with a real exFAT driver standing in for Windows.
# Run inside an ubuntu:24.04 container, project mounted at /src, --privileged:
#   docker run --rm --privileged -m 6g -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_commit_test.sh [scenario ...]
#
# Per scenario: build a tree, dump the synthesized volume, mount it read-write, make changes through the
# mount, then replay the changed sectors into the backend and commit onto the tree. The tree must end up
# identical to what the driver shows, and the rescanned volume must pass fsck.exfat.
set -u
apt-get update -qq >/dev/null
apt-get install -y -qq gcc exfatprogs exfat-fuse fuse3 >/dev/null 2>&1 || apt-get install -y -qq gcc exfatprogs exfat-fuse >/dev/null

gcc -Wall -Wextra -O1 -g ${EXTRA_CFLAGS:-} -o /tmp/commit_test tests/commit_test.c source/exfat_synth.c source/exfat_parse.c \
    source/overlay.c source/synth_commit.c || exit 1

P=/tmp/pristine   # starting tree, copied fresh for every scenario
T=/tmp/tree       # the "card"
W=/tmp/work
V=/mnt/vol
mkdir -p $V

mktree() {
    rm -rf $P; mkdir -p $P/a/b/c $P/empty_dir $P/many "$P/ünï cödé" $P/docs
    for i in $(seq 1 120); do echo "file $i" > $P/many/f_$i.txt; done
    head -c 100000 /dev/urandom > $P/a/rand.bin
    head -c 32768 /dev/urandom > $P/a/exact_cluster.bin
    head -c 32769 /dev/urandom > $P/a/cluster_plus_1.bin
    head -c 1 /dev/urandom > $P/a/b/one_byte
    : > $P/zero_len
    echo "hello" > "$P/ünï cödé/日本語.txt"
    head -c 3000000 /dev/urandom > $P/a/b/c/three_meg.bin
    head -c 70000 /dev/urandom > $P/docs/report.dat
    head -c 9000000 /dev/urandom > $P/a/rw_big.bin
    echo "keep me" > $P/docs/notes.txt
    if [ -n "${WITH_SVI:-}" ]; then
        mkdir -p "$P/System Volume Information" "$P/\$RECYCLE.BIN/S-1-5-21"
        echo guid > "$P/System Volume Information/IndexerVolumeGuid"; echo bin > "$P/\$RECYCLE.BIN/S-1-5-21/desktop.ini"
    fi
    truncate -s 64M $P/big_sparse.bin
    printf 'tail-of-big-file' | dd of=$P/big_sparse.bin bs=1 seek=$((64*1024*1024 - 16)) conv=notrunc 2>/dev/null
}

LOOP=""
mount_img() {  # img opts
    LOOP=$(losetup -f --show "$1") || return 1
    if mount -t exfat -o "$2" "$LOOP" $V 2>/dev/null; then return 0; fi
    if mount.exfat-fuse -o "$2" "$LOOP" $V 2>/dev/null; then return 0; fi
    losetup -d "$LOOP"; return 1
}
umount_img() {
    cd /src
    sync
    umount $V 2>/dev/null || fusermount -u $V 2>/dev/null
    losetup -d "$LOOP" 2>/dev/null
}

PASS=0; FAIL=0
check() {  # description, condition result (0 = ok)
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; FAIL=$((FAIL+1)); SCEN_FAIL=1; fi
}

# Compares the tree with what the driver shows for the modified image.
same_as_volume() {
    mount_img $W/vol.img ro || { echo "  cannot remount modified image"; return 1; }
    diff -rq -x big_sparse.bin -x 'System Volume Information' -x '$RECYCLE.BIN' $V $T >/tmp/diff.out 2>&1
    local rc=$?
    [ $rc -ne 0 ] && head -20 /tmp/diff.out | sed 's/^/    /'
    umount_img
    return $rc
}

scenario() {  # name, ops function, expected exit code, extra commit args...
    local name=$1 ops=$2 want=$3; shift 3
    SCEN_FAIL=0
    echo "== $name"
    rm -rf $W; mkdir -p $W
    [ -n "${KEEP:-}" ] || { rm -rf $T; cp -a --sparse=always $P $T; }
    /tmp/commit_test $T $W dump >/dev/null || { echo "  FAIL dump"; FAIL=$((FAIL+1)); return; }
    mount_img $W/vol.img rw || { echo "  FAIL cannot mount rw"; FAIL=$((FAIL+1)); return; }
    cd $V; $ops; cd /src
    umount_img
    cp -a --sparse=always $T /tmp/tree_before
    /tmp/commit_test $T $W commit $W/vol.img "$@" > $W/commit.out 2>&1
    local rc=$?
    sed -n '/REPORT/p;/zc?/p;/  seg /p;/ERROR/p;/changed on the Switch/p;/after commit/p' $W/commit.out | sed 's/^/  /'
    check "exit code $rc (wanted $want)" $([ $rc -eq $want ] && echo 0 || echo 1)
    if [ $rc -eq 0 ] && ! grep -q 'applied=0' $W/commit.out; then
        same_as_volume; check "tree matches the volume the host saw" $?
        fsck.exfat $W/post.img >/tmp/fsck.out 2>&1; local frc=$?
        [ $frc -ne 0 ] && tail -5 /tmp/fsck.out | sed 's/^/    /'
        check "rescanned volume passes fsck.exfat" $frc
        pat=nxusb-stage; [ -n "${ALLOW_STAGE0:-}" ] && pat=nxusb-stage-   # a pre-existing .nxusb-stage is expected there
        ls -A $T | grep -q "$pat" && stage_left=1 || stage_left=0
        check "no leftovers in stage" $stage_left
        cmp -s -n 1048576 $P/big_sparse.bin $T/big_sparse.bin; check "big untouched file intact (head)" $?
        cmp -s -i $((64*1024*1024-16)) $P/big_sparse.bin $T/big_sparse.bin; check "big untouched file intact (tail)" $?
    elif [ $rc -eq 3 ] && [ -z "${POKED:-}" ]; then
        diff -r /tmp/tree_before $T >/dev/null 2>&1; check "refused commit left the tree untouched" $?
    fi
    rm -rf /tmp/tree_before
    [ $SCEN_FAIL -eq 0 ] && PASS=$((PASS+1))
}

# ---- scenarios ----------------------------------------------------------------------------------

ops_copy_in() {
    rm -rf /tmp/incoming; mkdir -p /tmp/incoming/deep/er/est /tmp/incoming/emptydir
    for i in $(seq 1 150); do echo "new $i" > /tmp/incoming/deep/f_$i.txt; done
    head -c 5000000 /dev/urandom > /tmp/incoming/deep/er/five_meg.bin
    head -c 32768 /dev/urandom > /tmp/incoming/deep/er/est/one_cluster.bin
    : > /tmp/incoming/deep/er/est/empty.txt
    echo x > "/tmp/incoming/deep/ünï-新しい.txt"
    cp -a /tmp/incoming $V/incoming
    cp /tmp/incoming/deep/er/five_meg.bin $V/top_level_copy.bin
}
ops_delete() {
    rm $V/zero_len $V/a/rand.bin
    rm -r $V/a/b
    rm $V/many/f_1.txt $V/many/f_2.txt $V/many/f_3.txt
    rmdir $V/empty_dir
}
ops_rename_move() {
    mv $V/a/rand.bin $V/a/renamed.bin
    mv "$V/ünï cödé" "$V/ünï cödé 2"
    mv $V/docs/notes.txt $V/many/notes_moved.txt
    mv $V/a/b $V/docs/b_moved
    mv $V/many $V/a/many_in_a
}
ops_overwrite() {
    head -c 900000 /dev/urandom > $V/docs/report.dat           # bigger
    head -c 10 /dev/urandom > $V/a/cluster_plus_1.bin          # smaller
    echo "appended" >> $V/many/f_5.txt                          # append
    dd if=/dev/urandom of=$V/a/b/c/three_meg.bin bs=1 count=100 seek=2000000 conv=notrunc 2>/dev/null  # in place
    truncate -s 1000 $V/a/rand.bin
    : > $V/docs/notes.txt                                        # to zero length
    echo "was empty" > $V/zero_len                               # from zero length
}
ops_mixed() {
    mv $V/docs/report.dat $V/docs/Report.DAT                    # case-only rename
    mv $V/a/exact_cluster.bin $V/a/tmp_swap
    mv $V/a/cluster_plus_1.bin $V/a/exact_cluster.bin           # swap two names
    mv $V/a/tmp_swap $V/a/cluster_plus_1.bin
    rm $V/docs/notes.txt; echo "recreated" > $V/docs/notes.txt  # delete then recreate same name
    mkdir $V/newdir; mv $V/a/rand.bin $V/newdir/
    rm -r $V/many; mkdir $V/many; echo fresh > $V/many/only.txt
}
ops_many_files() {
    mkdir $V/crowd
    for i in $(seq 1 800); do echo "crowd $i" > $V/crowd/entry_number_$i.txt; done
}
ops_mass_delete() {
    rm $V/many/f_*.txt   # 120 files
}
ops_zc_copy() {
    head -c 40000000 /dev/urandom > $V/z_big1.bin
    mkdir $V/zd
    head -c 25000001 /dev/urandom > $V/zd/z_big2.bin
    head -c 33000000 /dev/urandom > $V/zd/z_big3.bin
    head -c 100000 /dev/urandom > $V/zd/small.bin
}
ops_zc_rewrite() { head -c 9000000 /dev/urandom > $V/a/rw_big.bin; }
ops_zc_partial() { dd if=/dev/urandom of=$V/a/rw_big.bin bs=1 count=1000 seek=4000000 conv=notrunc 2>/dev/null; }
ops_zc_append() { head -c 5000000 /dev/urandom >> $V/a/rw_big.bin; }
ops_zc_orphan() { head -c 12000000 /dev/urandom > $V/gone_again.bin; rm $V/gone_again.bin; echo ok > $V/after.txt; }
ops_zc_move() { head -c 12000000 /dev/urandom > $V/moveme.bin; mkdir $V/dest; mv $V/moveme.bin $V/dest/moved.bin; }
ops_noop() { :; }
ops_svi() {
    mkdir "$V/System Volume Information"
    echo guid > "$V/System Volume Information/IndexerVolumeGuid"
    mkdir "$V/\$RECYCLE.BIN"
    echo "after svi" > $V/after_svi.txt
}
ops_svi_only() {
    mkdir "$V/System Volume Information"
    echo guid > "$V/System Volume Information/IndexerVolumeGuid"
}
ops_names() {
    N=$(printf 'n%.0s' $(seq 1 250)); echo long > "$V/$N.txt"
    echo a > "$V/dots.in.name.v1.2.txt"; echo b > "$V/with  double  spaces.txt"
    echo c > "$V/UPPER lower MiXeD.TXT"; mkdir "$V/dir with space"; echo d > "$V/dir with space/日本語のファイル.txt"
    echo e > "$V/emoji 🎮 file.txt"
}
ops_stage_exists() {
    mv $V/docs/report.dat $V/docs/report_renamed.dat
    mv $V/a $V/a_renamed
}
ops_round2() {
    mv $V/incoming/deep $V/deep_moved
    rm -r $V/incoming
    echo "second round" > $V/second.txt
}
ops_rename_and_modify() {
    mv $V/docs/report.dat $V/docs/changed.dat
    echo "more" >> $V/docs/changed.dat
    mv $V/a/b/c/three_meg.bin $V/three.bin
    dd if=/dev/zero of=$V/three.bin bs=1 count=64 seek=1000 conv=notrunc 2>/dev/null
}
ops_touch_only() { touch $V/a/rand.bin; chmod 644 $V/docs/notes.txt 2>/dev/null; }
ops_delete_conflict() { rm $V/docs/notes.txt; }
ops_reformat() {
    cd /src; umount $V 2>/dev/null || fusermount -u $V 2>/dev/null
    mkfs.exfat -c 32768 $LOOP >/dev/null 2>&1
    mount -t exfat $LOOP $V 2>/dev/null || mount.exfat-fuse $LOOP $V
    cd $V
}

mktree
WANT="${*:-all}"
run() { [ "$WANT" = all ] || [[ " $WANT " == *" $1 "* ]]; }

run copy_in      && scenario copy_in      ops_copy_in      0
run delete       && scenario delete       ops_delete       0
run rename_move  && scenario rename_move  ops_rename_move  0
run overwrite    && scenario overwrite    ops_overwrite    0
run mixed        && scenario mixed        ops_mixed        0 yes
run rename_modify && scenario rename_modify ops_rename_and_modify 0
run names        && scenario names        ops_names        0
run svi          && scenario svi          ops_svi          0
run svi_only     && scenario svi_only     ops_svi_only     0
run many_files   && scenario many_files   ops_many_files   0
run noop         && scenario noop         ops_noop         0
run touch_only   && scenario touch_only   ops_touch_only   0
run mass_refused && scenario mass_refused ops_mass_delete  3
run mass_yes     && scenario mass_yes     ops_mass_delete  0 yes
run reformat     && scenario reformat     ops_reformat     3
run zc_copy      && { scenario zc_copy ops_zc_copy 0; grep -q "already in place" $W/commit.out; check "big files were renamed, not copied" $?; }
run zc_rewrite   && { scenario zc_rewrite ops_zc_rewrite 0; grep -q "already in place" $W/commit.out; check "rewritten big file was renamed" $?; }
run zc_partial   && scenario zc_partial ops_zc_partial 0
run zc_append    && scenario zc_append ops_zc_append 0
run zc_orphan    && scenario zc_orphan ops_zc_orphan 0
run zc_move      && { scenario zc_move ops_zc_move 0; grep -q "already in place" $W/commit.out; check "moved new file was renamed" $?; }

if run svi_real; then
    # Folders such as "System Volume Information" left on the card by other PCs must survive commits.
    WITH_SVI=1 mktree
    scenario svi_real ops_rename_move 0
    [ "$(cat "$T/System Volume Information/IndexerVolumeGuid" 2>/dev/null)" = guid ]; check "real System Volume Information kept" $?
    [ "$(cat "$T/\$RECYCLE.BIN/S-1-5-21/desktop.ini" 2>/dev/null)" = bin ]; check "real \$RECYCLE.BIN kept" $?
    mktree
fi

if run stage_exists; then
    # An earlier crashed commit left .nxusb-stage behind (it is visible to the host). A new commit must
    # pick another stage name and leave those files alone.
    mkdir -p $P/.nxusb-stage; echo precious > $P/.nxusb-stage/p7
    ALLOW_STAGE0=1 scenario stage_exists ops_stage_exists 0
    [ "$(cat $T/.nxusb-stage/p7 2>/dev/null)" = precious ]; check "old stage content untouched" $?
    rm -rf $P/.nxusb-stage
fi

if run round2; then
    # Two commits in a row on the same card: the second starts from the rescanned volume.
    scenario round2_a ops_copy_in 0
    KEEP=1 scenario round2_b ops_round2 0
    unset KEEP
fi

if run partial; then
    # A copy cut short: only some sector runs arrive. Whatever the outcome, it must not crash, hang or
    # leave the card in a state the rescan cannot read.
    echo "== partial"
    SCEN_FAIL=0
    rm -rf $T $W; mkdir -p $W; cp -a --sparse=always $P $T
    /tmp/commit_test $T $W dump >/dev/null
    mount_img $W/vol.img rw; cd $V; ops_copy_in; cd /src; umount_img
    /tmp/commit_test $T $W commit $W/vol.img subset yes > $W/commit.out 2>&1; rc=$?
    sed -n '/REPORT/p' $W/commit.out | sed 's/^/  /'
    check "exit code $rc is 0, 3 or 4" $([ $rc -eq 0 -o $rc -eq 3 -o $rc -eq 4 ] && echo 0 || echo 1)
    if grep -q 'applied=1' $W/commit.out; then
        fsck.exfat $W/post.img >/tmp/fsck.out 2>&1; check "rescanned volume passes fsck.exfat" $?
    fi
    [ $SCEN_FAIL -eq 0 ] && PASS=$((PASS+1))
fi

if run nospace; then
    # The card is nearly full: a commit needs room to stage the new contents before it touches anything.
    echo "== nospace"
    SCEN_FAIL=0
    rm -rf $T $W; mkdir -p $W $T
    mount -t tmpfs -o size=20m tmpfs $T
    cp -a --sparse=always $P/. $T/
    /tmp/commit_test $T $W dump >/dev/null
    mount_img $W/vol.img rw; head -c 15000000 /dev/urandom > $V/big_new.bin; umount_img
    cp -a $T /tmp/tree_before
    /tmp/commit_test $T $W commit $W/vol.img > $W/commit.out 2>&1; rc=$?
    sed -n '/REPORT/p' $W/commit.out | sed 's/^/  /'
    check "refused (exit $rc, wanted 3)" $([ $rc -eq 3 ] && echo 0 || echo 1)
    [ ! -e $T/big_new.bin ]; check "nothing was created on the card" $?
    ls -A $T | grep -q nxusb-stage && stage_left=1 || stage_left=0; check "no stage directory left" $stage_left
    diff -r /tmp/tree_before $T >/dev/null 2>&1; check "tree untouched" $?
    rm -rf /tmp/tree_before
    umount $T
    [ $SCEN_FAIL -eq 0 ] && PASS=$((PASS+1))
fi

if run pump; then
    # The commit calls the pump callback while it works, so the USB host keeps being answered.
    scenario pump ops_copy_in 0 pump yes
    n=$(sed -n 's/^PUMP calls=//p' $W/commit.out); [ "${n:-0}" -gt 10 ]; check "pump called while committing (${n:-0} times)" $?
fi

if run cancel; then
    # Cancelled while the new file data is staged: the card must be exactly as before, no stage folder.
    scenario cancel ops_copy_in 3 cancel yes
    grep -q "cancelled while copying" $W/commit.out; check "commit reported the cancel" $?
fi

if run hidden; then
    # The app's own folder is left out of the volume: the PC does not see it and a commit does not touch it.
    echo "== hidden"
    SCEN_FAIL=0
    rm -rf $T $W; mkdir -p $W; cp -a --sparse=always $P $T
    mkdir -p $T/switch/app_private; echo "scratch" > $T/switch/app_private/overlay.bin; echo "other" > $T/switch/other.txt
    cp -a $T/switch /tmp/switch_before
    /tmp/commit_test $T $W dump hide=switch/app_private >/dev/null
    mount_img $W/vol.img rw
    [ -d $V/switch ] && [ ! -e $V/switch/app_private ] && [ -f $V/switch/other.txt ]; check "PC sees switch/ but not the hidden folder" $?
    echo "new" > $V/switch/added.txt; rm $V/switch/other.txt
    umount_img
    /tmp/commit_test $T $W commit $W/vol.img hide=switch/app_private > $W/commit.out 2>&1; rc=$?
    sed -n '/REPORT/p' $W/commit.out | sed 's/^/  /'
    check "exit code $rc (wanted 0)" $([ $rc -eq 0 ] && echo 0 || echo 1)
    [ "$(cat $T/switch/app_private/overlay.bin)" = scratch ]; check "hidden folder untouched" $?
    [ -f $T/switch/added.txt ] && [ ! -e $T/switch/other.txt ]; check "other changes applied" $?
    rm -rf /tmp/switch_before
    [ $SCEN_FAIL -eq 0 ] && PASS=$((PASS+1))
fi

if run conflict; then
    # The host deletes a file that the Switch modified after the scan: must be refused.
    POKED=1 scenario conflict ops_delete_conflict 3 poke=docs/notes.txt
    [ -f $T/docs/notes.txt ]; check "file still on the card" $?
fi

echo
echo "scenarios passed: $PASS, checks failed: $FAIL"
[ $FAIL -eq 0 ]
