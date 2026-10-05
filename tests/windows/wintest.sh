#!/bin/bash
# Usage: wintest.sh <jobname>
# Real-Windows end to end test of the write path:
#   A. dump the synthesized volume of a tree, wrap as VHD, let Windows (elevated server) mount it and run win_ops.ps1
#   B. take the modified VHD, replay the changed sectors into the backend, commit onto the tree,
#      compare the tree with the manifest Windows wrote.
set -e
NAME=$1
PROJ=$(cd "$(dirname "$0")/../.." && pwd)
T="${WINTEST_DIR:?set WINTEST_DIR to a scratch folder holding make_vhd.ps1 and server.ps1}"
J="$T/vhdsrv/jobs"
mkdir -p "$T/vhd"
cd "$PROJ"
DOCKER="docker run --rm -v $(pwd -W):/src -v $(cygpath -w "$T/vhd"):/out -v $(cygpath -w "$J"):/jobs -w /src ubuntu:24.04"
BUILD='apt-get update -qq >/dev/null; apt-get install -y -qq gcc >/dev/null 2>&1;
  gcc -O1 -o /tmp/ct tests/commit_test.c source/exfat_synth.c source/exfat_parse.c source/overlay.c source/synth_commit.c 2>/dev/null'

echo "--- phase A: build volume"
MSYS_NO_PATHCONV=1 $DOCKER bash -c "$BUILD
  if [ ! -d /out/tree_pristine ]; then
    mkdir -p /out/tree_pristine/sub /out/tree_pristine/docs
    echo hi > /out/tree_pristine/a.txt; echo yo > /out/tree_pristine/sub/b.txt; echo keep > /out/tree_pristine/docs/notes.txt
    head -c 100000 /dev/urandom > /out/tree_pristine/docs/blob.bin
    head -c 70000 /dev/urandom > /out/tree_pristine/sub/seventy_k.bin
  fi
  rm -rf /out/tree /out/work; cp -a /out/tree_pristine /out/tree; mkdir -p /out/work
  /tmp/ct /out/tree /out/work dump | tail -1
  cp /out/work/vol.img /out/vol.img"
rm -f "$J/$NAME.result.txt" "$J/manifest_windows.txt"
powershell.exe -NoProfile -File "$(cygpath -w "$T/make_vhd.ps1")" -Img "$(cygpath -w "$T/vhd/vol.img")" -Vhd "$(cygpath -w "$J/$NAME.vhd")"
cp "$T/win_ops.ps1" "$J/$NAME.ops.ps1"
touch "$J/$NAME.go"
echo "--- phase A: waiting for Windows"
for i in $(seq 1 300); do
  [ -f "$J/$NAME.result.txt" ] && rm -f "$J/$NAME.go"   # the server has picked the job up: no re-runs
  if grep -q '^DONE' "$J/$NAME.result.txt" 2>/dev/null; then break; fi
  sleep 2
done
cat "$J/$NAME.result.txt"

echo "--- phase B: replay and commit"
MSYS_NO_PATHCONV=1 $DOCKER bash -c "$BUILD
  SECT=\$(( \$(stat -c %s /out/vol.img) / 512 ))
  dd if=/jobs/$NAME.vhd of=/out/modified.img bs=512 skip=2048 count=\$SECT status=none
  /tmp/ct /out/tree /out/work commit /out/modified.img yes 2>&1 | grep -v '^  | \(delete\|mkdir\|create\|rewrite\|move\|rmdir\)' | tail -15
  cd /out/tree
  { find . -mindepth 1 -type d | sed 's|^\./|D /|'; find . -type f | while read -r f; do printf 'F /%s %s %s\n' \"\${f#./}\" \"\$(stat -c %s \"\$f\")\" \"\$(sha256sum \"\$f\" | cut -d' ' -f1)\"; done; } | LC_ALL=C sort > /out/manifest_tree.txt
  tr -d '\r' < /jobs/manifest_windows.txt | LC_ALL=C sort > /out/manifest_win_sorted.txt
  echo 'windows entries:' \$(wc -l < /out/manifest_win_sorted.txt) ' tree entries:' \$(wc -l < /out/manifest_tree.txt)
  if diff /out/manifest_win_sorted.txt /out/manifest_tree.txt > /out/manifest.diff; then echo 'MANIFESTS IDENTICAL'; else echo 'MANIFEST DIFFERENCES:'; head -30 /out/manifest.diff; fi
  ls -A /out/tree | grep nxusb-stage && echo 'STAGE LEFTOVER' || true"
