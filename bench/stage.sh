#!/bin/sh
# Stage and build one labelled configuration in its own LF directory.
# Run inside WSL/Linux from the repository root:
#   sh bench/stage.sh normal|test|orig DEST [REV]
# normal : modified kernel, production build        (make -B xv6.img fs.img)
# test   : modified kernel, fault-injection build   (make -B SHADOW_TEST=1 ...)
# orig   : original xv6 baseline = commit fd7d454 (stock x86 xv6 plus the
#          course add/hello syscalls) with FSSIZE raised 1000 -> 2000 to match
#          the modified tree, and the portable benchmark spbench.c (ORIG_XV6).
# REV (a commit) exports that commit with git archive. Without REV the
# current working tree is copied, including uncommitted and untracked files;
# STAGE-INFO.txt then says dirty_tree=yes. Final evidence uses REV=HEAD.
# Each configuration builds in its own directory, so building one can never
# replace another configuration's disk images.
set -eu
CONFIG=$1; DEST=$2; REV=${3:-}
REPO=$(cd "$(dirname "$0")/.." && pwd)
case $CONFIG in normal|test|orig) ;; *) echo "bad config $CONFIG" >&2; exit 2;; esac
G="git -C $REPO -c core.autocrlf=true"
rm -rf "$DEST"; mkdir -p "$DEST"
if [ "$CONFIG" = orig ]; then
  git -C "$REPO" -c core.autocrlf=false -c core.eol=lf archive fd7d454 | tar -x -C "$DEST"
  SRC=$(git -C "$REPO" rev-parse fd7d454)
elif [ -n "$REV" ]; then
  git -C "$REPO" -c core.autocrlf=false -c core.eol=lf archive "$REV" | tar -x -C "$DEST"
  SRC=$(git -C "$REPO" rev-parse "$REV")
else
  # Working tree: tracked + untracked (not ignored); strip CR from text files.
  (cd "$REPO" && $G ls-files -co --exclude-standard) | while IFS= read -r f; do
    [ -f "$REPO/$f" ] || continue
    mkdir -p "$DEST/$(dirname "$f")"
    if grep -Iq . "$REPO/$f" 2>/dev/null; then tr -d '\r' < "$REPO/$f" > "$DEST/$f"
    else cp "$REPO/$f" "$DEST/$f"; fi
    [ -x "$REPO/$f" ] && chmod +x "$DEST/$f" || true
  done
  chmod +x "$DEST"/*.pl "$DEST"/cuth "$DEST"/runoff* 2>/dev/null || true
  SRC="worktree@$(git -C "$REPO" rev-parse HEAD)"
fi
cd "$DEST"
if [ "$CONFIG" = orig ]; then
  sed -i 's/^#define FSSIZE .*/#define FSSIZE       2000  \/\/ baseline: raised from 1000 to match the modified tree/' param.h
  { echo '#define ORIG_XV6 1'; tr -d '\r' < "$REPO/spbench.c"; } > spbench.c
  awk '{print} /^\t_addtest\\$/{print "\t_spbench\\"}' Makefile > Makefile.new
  mv Makefile.new Makefile
  grep -q _spbench Makefile
  # Host-side test harness only (runs usertests on the baseline image).
  mkdir -p tests && tr -d '\r' < "$REPO/tests/run-spec-tests.py" > tests/run-spec-tests.py
  BUILD="make -B xv6.img fs.img"
elif [ "$CONFIG" = test ]; then
  BUILD="make -B SHADOW_TEST=1 xv6.img fs.img"
else
  BUILD="make -B xv6.img fs.img"
fi
# Hash the final source tree after baseline patches, before any build output.
# This includes the benchmark, harness, Makefile and configuration actually used.
TREEHASH=$(find . -type f ! -name STAGE-INFO.txt -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)
DIRTY=no
[ -n "$($G status --porcelain)" ] && DIRTY=yes
{
  echo "config=$CONFIG"
  echo "source=$SRC"
  echo "branch_head=$(git -C "$REPO" rev-parse HEAD)"
  echo "dirty_tree=$DIRTY"
  echo "staged_source_sha256=$TREEHASH"
  echo "staged_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > STAGE-INFO.txt
if [ "$CONFIG" = orig ]; then
  echo "baseline_patch=FSSIZE 1000->2000; +_spbench (compiled with ORIG_XV6)" >> STAGE-INFO.txt
fi
echo "build_command=$BUILD" >> STAGE-INFO.txt
$BUILD > build.txt 2>&1 || { tail -30 build.txt; exit 1; }
echo "test_only_kernel_symbols=$(grep -c 'shadow_test\|shadow_crash_point' kernel.sym || true)" >> STAGE-INFO.txt
echo "fs_img_sha256=$(sha256sum fs.img | cut -c1-16) xv6_img_sha256=$(sha256sum xv6.img | cut -c1-16)" >> STAGE-INFO.txt
cat STAGE-INFO.txt
