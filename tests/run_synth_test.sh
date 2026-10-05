#!/bin/bash
# Run inside an ubuntu:24.04 container, with the project mounted at /src and --privileged:
#   docker run --rm --privileged -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_synth_test.sh
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq gcc exfatprogs exfat-fuse fuse3 >/dev/null 2>&1 || apt-get install -y -qq gcc exfatprogs exfat-fuse >/dev/null

gcc -Wall -Wextra -O1 -g -o /tmp/synth_test tests/synth_test.c source/exfat_synth.c source/exfat_parse.c source/overlay.c source/synth_commit.c

# Test tree: nesting, many files in one dir, unicode, empty files/dirs, a >4 GiB sparse file.
T=/tmp/tree
rm -rf $T; mkdir -p $T/a/b/c $T/empty_dir $T/many "$T/ünï cödé" $T/long
for i in $(seq 1 700); do echo "file $i" > $T/many/f_$i.txt; done
head -c 100000 /dev/urandom > $T/a/rand.bin
head -c 32768 /dev/urandom > $T/a/exact_cluster.bin
head -c 32769 /dev/urandom > $T/a/cluster_plus_1.bin
head -c 1 /dev/urandom > $T/a/b/one_byte
: > $T/zero_len
echo "hello" > "$T/ünï cödé/日本語.txt"
echo "emoji" > "$T/ünï cödé/🎮.txt"
N=$(printf 'x%.0s' $(seq 1 200)); echo hi > "$T/long/$N.txt"
head -c 3000000 /dev/urandom > $T/a/b/c/three_meg.bin
truncate -s 5G $T/big_sparse.bin
printf 'tail-of-big-file' | dd of=$T/big_sparse.bin bs=1 seek=$((5*1024*1024*1024 - 16)) conv=notrunc 2>/dev/null

/tmp/synth_test $T /tmp/vol.img 2>&1 | tail -3
echo "--- fsck.exfat"
fsck.exfat -v /tmp/vol.img 2>&1 | tail -20
echo "--- mount + compare"
mkdir -p /mnt/vol
LOOP=$(losetup -f --show -r /tmp/vol.img 2>/dev/null || true)
if [ -n "$LOOP" ] && mount -t exfat -o ro "$LOOP" /mnt/vol 2>/dev/null; then echo "(kernel exfat via $LOOP)"
elif [ -n "$LOOP" ] && mount.exfat-fuse -o ro "$LOOP" /mnt/vol; then echo "(exfat-fuse via $LOOP)"
else echo "could not mount (loop='$LOOP')"; exit 1; fi
ls /mnt/vol
diff -rq -x big_sparse.bin $T /mnt/vol && echo "TREE IDENTICAL (excluding big file)"
cmp -n 1048576 $T/big_sparse.bin /mnt/vol/big_sparse.bin && echo "BIG FILE HEAD IDENTICAL"
cmp -i 5368709100:5368709100 $T/big_sparse.bin /mnt/vol/big_sparse.bin && echo "BIG FILE TAIL IDENTICAL (past 4 GiB)"
df -h /mnt/vol | tail -1
# Unmount cleanly, otherwise the FUSE mount keeps the container from exiting.
cd /
umount /mnt/vol 2>/dev/null || fusermount -u /mnt/vol 2>/dev/null || true
losetup -d "$LOOP" 2>/dev/null || true
