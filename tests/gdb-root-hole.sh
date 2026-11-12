#!/bin/sh
# Record a GDB session showing defect D1 (root-directory hole) and its fix.
# Usage (WSL/Linux, repository root, gdb installed):
#   sh tests/gdb-root-hole.sh QEMU WORKDIR OUTDIR
# Builds the test configuration with the root padded to exactly 32 entries,
# once with the stock mkfs rounding (bug) and once with the repository mkfs.c
# (fix). For each, QEMU starts halted (-S) with a gdb stub, gdb attaches,
# breakpoints log every bmap() call that fills root block 1 and every
# fs_finish(), and the guest runs spectest. Transcripts: OUTDIR/gdb-*.txt
set -eu
QEMU=$1; WORK=$2; OUT=$3
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
# Reuse the padding + stock-mkfs build of the regression script.
sh "$REPO/tests/regress-root-hole.sh" "$QEMU" "$WORK" "$OUT/gdb-prep-logs" > "$OUT/gdb-prep.txt" 2>&1 || true
cd "$WORK"

cat > root-hole.gdb <<'GDB'
set pagination off
set confirm off
set architecture i386
file kernel
target remote :26000
set $hit = 0
# Fires when block index 1 of the root directory has no disk block yet.
break bmap if ip->inum == 1 && bn == 1 && ip->addrs[1] == 0
commands
  silent
  set $hit = $hit + 1
  printf "\n[gdb] hit %d: bmap(root inode 1, bn=1) with addrs[1]=0, cached size=%d, current pid=%d spec=%d\n", $hit, ip->size, cpus[0].proc->pid, cpus[0].proc->spec
  printf "[gdb]   size %d covers block 1 although it is not allocated: %s\n", ip->size, ip->size > 512 ? "HOLE (size already > 512; writei will not iupdate)" : "normal extension (size grows, writei iupdates)"
  bt 5
  continue
end
break fs_finish
commands
  silent
  printf "\n[gdb] fs_finish(committed=%d) by pid %d: completion cleanup, calls inode_invalidate()\n", committed, cpus[0].proc->pid
  continue
end
continue
GDB

record() {  # label
  label=$1
  make -B SHADOW_TEST=1 xv6.img fs.img > build-$label.txt 2>&1
  echo "root after mkfs: $(python3 rootinfo.py)" > "$OUT/gdb-$label.txt"
  rm -f guest-in; mkfifo guest-in
  ( sleep 25; printf 'spectest\n'; sleep 150 ) > guest-in &
  feeder=$!
  "$QEMU" -display none -monitor none -serial stdio -no-reboot -smp 1 -m 512 \
    -drive file=xv6.img,index=0,format=raw -drive file=fs.img,index=1,format=raw \
    -S -gdb tcp::26000 < guest-in > "$OUT/gdb-$label-serial.txt" 2>&1 &
  qemu=$!
  sleep 2
  timeout 170 gdb -q -batch -x root-hole.gdb >> "$OUT/gdb-$label.txt" 2>&1 || true
  kill $qemu $feeder 2>/dev/null || true
  wait 2>/dev/null || true
  echo "---- guest serial result ----" >> "$OUT/gdb-$label.txt"
  tr -d '\r' < "$OUT/gdb-$label-serial.txt" | grep -a "FAIL\|SPEC TESTS" >> "$OUT/gdb-$label.txt" || true
}

# 1. Bug: stock rounding (regress-root-hole.sh left the fixed mkfs.c; restore stock)
python3 - <<'PY'
s = open("mkfs.c").read()
a = s.index("  off = ((off + BSIZE - 1) / BSIZE) * BSIZE;")
b = s.index("  winode(rootino, &din);", a)
s = s[:a] + "  off = ((off/BSIZE) + 1) * BSIZE;\n  din.size = xint(off);\n" + s[b:]
open("mkfs.c", "w").write(s)
PY
record stock-mkfs
# 2. Fix: repository mkfs.c
tr -d '\r' < "$REPO/mkfs.c" > mkfs.c
record fixed-mkfs
echo "transcripts: $OUT/gdb-stock-mkfs.txt $OUT/gdb-fixed-mkfs.txt"
