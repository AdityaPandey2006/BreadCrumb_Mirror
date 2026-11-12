#!/bin/sh
# Step 10 correctness matrix. Runs every suite on the staged builds and
# records PASS/FAIL per run without stopping at the first failure.
# Usage (WSL/Linux, repository root, builds staged with bench/stage.sh):
#   sh tests/run-matrix.sh QEMU NORMAL_DIR TEST_DIR ORIG_DIR LOGDIR [quick]
# Writes LOGDIR/matrix.tsv (suite, build, cpus, repeat, result, seconds, log)
# and one raw serial log per run.
set -u
QEMU=$1; NORMAL=$2; TEST=$3; ORIG=$4; LOGS=$5; QUICK=${6:-}
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$LOGS"
TSV=$LOGS/matrix.tsv
printf 'suite\tbuild\tcpus\trepeat\tresult\tseconds\tlog\n' > "$TSV"

run() {  # build_dir build_name suite cpus repeat timeout
  dir=$1; name=$2; suite=$3; cpus=$4; rep=$5; to=$6
  log=$LOGS/$suite-$name-cpu$cpus-x$rep.txt
  start=$(date +%s)
  if (cd "$dir" && timeout $((to * rep + 600)) python3 tests/run-spec-tests.py --qemu "$QEMU" \
        --suite "$suite" --cpus "$cpus" --repeat "$rep" --timeout "$to" --log "$log" \
        > "$log.stdout" 2>&1); then res=PASS; else res=FAIL; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$suite" "$name" "$cpus" "$rep" "$res" \
    "$(( $(date +%s) - start ))" "$(basename "$log")" >> "$TSV"
  echo "$res  $suite $name cpus=$cpus x$rep"
}

R=3; [ -n "$QUICK" ] && R=1
# Fault-injection (SHADOW_TEST=1) build
for c in 1 2 3 4; do run "$TEST" test shadow $c $R 300; done
for c in 1 3; do run "$TEST" test conflict $c $((R * 3)) 300; done
for c in 1 2 3 4; do run "$TEST" test failure $c $R 300; done
for c in 1 4; do run "$TEST" test namespace $c $R 300; done
for c in 1 3; do run "$TEST" test console $c $R 300; done
for c in 1 3; do run "$TEST" test pipe $c $R 300; done
for c in 1 3; do run "$TEST" test crash $c 1 300; done
for c in 1 3; do run "$TEST" test namespace-crash $c 1 300; done
for c in 1 3; do run "$TEST" test output-crash $c $R 300; done
run "$TEST" test lifecycle 1 1 300
run "$TEST" test usertests 1 1 1500
# Production build
for c in 1 3; do run "$NORMAL" normal lifecycle $c 1 300; done
for c in 1 3; do run "$NORMAL" normal failure $c $R 300; done
for c in 1 3; do run "$NORMAL" normal drainfull $c $R 120; done
run "$NORMAL" normal namespace 3 $R 300
run "$NORMAL" normal conflict 3 $R 300
for c in 1 2; do run "$NORMAL" normal usertests $c 1 1500; done
# Original-xv6 baseline sanity (FSSIZE 2000 + spbench only)
run "$ORIG" orig usertests 1 1 1500
# Root-directory hole regression (D1) and host agent tests
start=$(date +%s)
if sh "$REPO/tests/regress-root-hole.sh" "$QEMU" "$LOGS/../roothole-work" "$LOGS" > "$LOGS/root-hole-regression.txt" 2>&1
then res=PASS; else res=FAIL; fi
printf 'root-hole-regression\ttest\t1\t1\t%s\t%s\troot-hole-regression.txt\n' "$res" "$(( $(date +%s) - start ))" >> "$TSV"
echo "$res  root-hole regression"
start=$(date +%s)
if (cd "$REPO" && python3 -m unittest discover -s agent > "$LOGS/agent-unittest.txt" 2>&1)
then res=PASS; else res=FAIL; fi
printf 'agent-unittest\thost\t-\t1\t%s\t%s\tagent-unittest.txt\n' "$res" "$(( $(date +%s) - start ))" >> "$TSV"
echo "$res  agent unit tests"
echo "matrix written to $TSV"
awk -F '\t' 'NR > 1 {
  if ($5 == "PASS") passed++; else failed++;
} END {
  printf "PASS rows: %d\nFAIL rows: %d\n", passed, failed;
  exit (failed != 0);
}' "$TSV"
