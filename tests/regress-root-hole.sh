#!/bin/sh
# Regression for the Step-10 "root directory hole" bug (see
# design/step10-implementation.md, defect D1).
#
# Stock xv6 mkfs rounded the root directory size up by one whole block even
# when the last block was exactly full, leaving size > allocated blocks
# (addrs[k] == 0). The first lookup then allocated that block only in the
# cached inode (bmap without iupdate); Step 6's inode_invalidate() on abort
# reloaded the on-disk inode and lost every entry in that block.
#
# The script stages the CURRENT tree (test build), adds empty padding files until
# the root holds exactly a multiple of 32 entries (the failing layout), then:
#   1. builds with the stock mkfs rounding restored -> lifecycle suite expected to FAIL
#   2. builds with the repository mkfs.c            -> lifecycle suite must PASS
# Usage (WSL/Linux, repository root):
#   sh tests/regress-root-hole.sh QEMU WORKDIR LOGDIR
set -eu
QEMU=$1; WORK=$2; LOGS=$3
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$LOGS"
sh "$REPO/bench/stage.sh" test "$WORK" > /dev/null
cd "$WORK"
cat > rootinfo.py <<'PY'
import struct
img = open("fs.img", "rb").read()
inodestart = struct.unpack_from("<7I", img, 512)[5]
din = img[inodestart * 512 + 64: inodestart * 512 + 128]      # inode 1 = root
size = struct.unpack_from("<I", din, 8)[0]
addrs = [a for a in struct.unpack_from("<13I", din, 12)[:12] if a]
used = sum(1 for b in addrs for i in range(0, 512, 16)
           if struct.unpack_from("<H", img, b * 512 + i)[0])
print(f"root size={size} allocated_blocks={len(addrs)} entries={used}")
PY
entries=$(python3 rootinfo.py | sed 's/.*entries=//')
need=$(( (32 - entries % 32) % 32 ))
echo "current tree: $(python3 rootinfo.py); adding $need empty padding files"
i=0
padding=""
while [ $i -lt $need ]; do
  : > rootpad$i
  padding="$padding rootpad$i"
  i=$((i + 1))
done
# Only directory entries matter here. Executable padding can exhaust the
# 2000-block image when a new test moves the root just past a block boundary.
printf '\nROOTPAD :=%s\n' "$padding" >> Makefile
python3 - <<'PYPAD'
from pathlib import Path
p = Path('Makefile')
s = p.read_text().replace('fs.img: mkfs README $(UPROGS)',
                          'fs.img: mkfs README $(UPROGS) $(ROOTPAD)')
s = s.replace('./mkfs fs.img README $(UPROGS)',
              './mkfs fs.img README $(UPROGS) $(ROOTPAD)')
p.write_text(s)
PYPAD
# 1. stock rounding restored (bug present)
python3 - <<'PY'
s = open("mkfs.c").read()
a = s.index("  off = ((off + BSIZE - 1) / BSIZE) * BSIZE;")
b = s.index("  winode(rootino, &din);", a)
s = s[:a] + "  off = ((off/BSIZE) + 1) * BSIZE;\n  din.size = xint(off);\n" + s[b:]
open("mkfs.c", "w").write(s)
PY
make -B SHADOW_TEST=1 xv6.img fs.img > build-stock-mkfs.txt 2>&1
echo "stock mkfs: $(python3 rootinfo.py)"
if python3 tests/run-spec-tests.py --qemu "$QEMU" --suite lifecycle --cpus 1 --log "$LOGS/root-hole-stock-mkfs.txt" > /dev/null 2>&1; then
  echo "NOTE: stock mkfs layout did not reproduce the failure"
else
  echo "stock mkfs: lifecycle FAILED as expected: $(grep -a 'FAIL:' "$LOGS/root-hole-stock-mkfs.txt" | head -1)"
fi
# 2. repository mkfs.c (fixed)
tr -d '\r' < "$REPO/mkfs.c" > mkfs.c
make -B SHADOW_TEST=1 xv6.img fs.img > build-fixed-mkfs.txt 2>&1
echo "fixed mkfs: $(python3 rootinfo.py)"
python3 tests/run-spec-tests.py --qemu "$QEMU" --suite lifecycle --cpus 1 --log "$LOGS/root-hole-fixed-mkfs.txt" > /dev/null
echo "PASS: root-hole regression (fixed mkfs, root entries a multiple of 32)"
