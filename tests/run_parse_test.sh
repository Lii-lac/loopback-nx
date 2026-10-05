#!/bin/bash
# Checks the exFAT parser against volumes made by mkfs.exfat and filled by a real driver: other cluster
# sizes, fragmented files, directories that outgrow a cluster.
#   docker run --rm --privileged -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_parse_test.sh
set -u
apt-get update -qq >/dev/null
apt-get install -y -qq gcc python3 exfatprogs exfat-fuse fuse3 >/dev/null 2>&1 || apt-get install -y -qq gcc python3 exfatprogs exfat-fuse >/dev/null
gcc -Wall -Wextra -O1 -g -o /tmp/parse_test tests/parse_test.c source/exfat_parse.c || exit 1

V=/mnt/vol; mkdir -p $V
FAIL=0

mount_img() {
    LOOP=$(losetup -f --show "$1") || return 1
    mount -t exfat -o "$2" "$LOOP" $V 2>/dev/null || mount.exfat-fuse -o "$2" "$LOOP" $V 2>/dev/null || { losetup -d "$LOOP"; return 1; }
}
umount_img() { cd /src; sync; umount $V 2>/dev/null || fusermount -u $V 2>/dev/null; losetup -d "$LOOP" 2>/dev/null; }

for CL in 4096 32768 262144; do
    echo "== cluster size $CL"
    IMG=/tmp/p.img; rm -f $IMG; truncate -s 400M $IMG
    mkfs.exfat -c $CL $IMG >/dev/null || { echo "  mkfs failed"; FAIL=1; continue; }
    mount_img $IMG rw || { echo "  mount failed"; FAIL=1; continue; }
    (
        cd $V
        # Two files grown in turn interleave their clusters: both end up fragmented.
        : > frag_a.bin; : > frag_b.bin
        for i in $(seq 1 120); do
            head -c $CL /dev/urandom >> frag_a.bin
            head -c $CL /dev/urandom >> frag_b.bin
            sync -f frag_a.bin
        done
        mkdir -p deep/er/est big_dir
        for i in $(seq 1 600); do echo "entry $i" > big_dir/some_fairly_long_file_name_$i.txt; done
        head -c 2500000 /dev/urandom > deep/er/est/two_and_a_half.bin
        : > deep/empty; echo x > "deep/ünï 日本.txt"
        rm big_dir/some_fairly_long_file_name_1*.txt
        N=$(printf 'z%.0s' $(seq 1 240)); echo long > "deep/$N"
    )
    umount_img
    /tmp/parse_test $IMG /tmp/extract_out > /tmp/parse.out 2>&1 || { cat /tmp/parse.out; echo "  FAIL parse"; FAIL=1; continue; }
    sed 's/^/  /' /tmp/parse.out
    mount_img $IMG ro || { echo "  remount failed"; FAIL=1; continue; }
    if diff -r $V /tmp/extract_out >/tmp/diff.out 2>&1; then echo "  ok   extracted tree identical to the driver's view"
    else echo "  FAIL extracted tree differs"; head /tmp/diff.out | sed 's/^/    /'; FAIL=1; fi
    umount_img
    rm -rf /tmp/extract_out
done

# A corrupted entry must fail the parse, not produce a partial tree.
echo "== corrupted entry set"
IMG=/tmp/p.img; rm -f $IMG; truncate -s 16M $IMG; mkfs.exfat -c 4096 $IMG >/dev/null
mount_img $IMG rw; echo hi > $V/file.txt; umount_img
python3 - <<'EOF' 2>/dev/null || echo "  (python3 missing, skipped)"
import struct
d = bytearray(open('/tmp/p.img','rb').read())
heap = struct.unpack_from('<I', d, 88)[0] * 512
root = struct.unpack_from('<I', d, 96)[0]
spc = 512 << d[109]
off = heap + (root - 2) * spc
for i in range(0, spc, 32):
    if d[off + i] == 0x85:
        d[off + i + 2] ^= 0xFF       # break the entry-set checksum
        break
open('/tmp/p.img','wb').write(d)
EOF
if /tmp/parse_test /tmp/p.img /tmp/extract_bad >/tmp/parse.out 2>&1; then echo "  FAIL parse accepted a bad checksum"; FAIL=1
else echo "  ok   rejected: $(head -1 /tmp/parse.out)"; fi

[ $FAIL -eq 0 ] && echo "ALL OK" || echo "FAILURES"
exit $FAIL
