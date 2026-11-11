#!/bin/sh
# Step 10 end-to-end reproduction: environment record, staged builds,
# correctness matrix, measurements, figures and demonstrations.
# Run inside WSL/Linux from the repository root:
#   sh bench/run-all.sh [QEMU] [WORK] [REV]
# QEMU defaults to ~/qemu/build/qemu-system-i386, WORK to ~/bc10/final.
# REV (optional) builds that commit instead of the working tree.
# Matplotlib for the figures: python3 -m venv WORK/venv &&
#   WORK/venv/bin/pip install -r bench/requirements.txt
# Total time on the reference machine: about 1.5 hours.
set -eu
QEMU=${1:-$HOME/qemu/build/qemu-system-i386}
WORK=${2:-$HOME/bc10/final}
REV=${3:-}
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$WORK"
cd "$REPO"
sh bench/env-report.sh "$QEMU" > "$WORK/environment.txt"
for c in normal test orig; do
  sh bench/stage.sh $c "$WORK/$c" $REV > "$WORK/stage-$c.txt"
done
# Correctness first (fix defects before measuring), never concurrently with timing.
sh tests/run-matrix.sh "$QEMU" "$WORK/normal" "$WORK/test" "$WORK/orig" "$WORK/matrix"
# Measurements (1 CPU micro-benchmarks, 1/2/4 CPU concurrency, memory).
python3 bench/run-bench.py --qemu "$QEMU" --orig "$WORK/orig" --normal "$WORK/normal" \
  --test "$WORK/test" --out "$WORK/results"
# Figures and summary tables from the raw CSVs.
if [ -x "$WORK/venv/bin/python" ]; then
  "$WORK/venv/bin/python" bench/plot.py --res "$WORK/results" --out "$WORK/figures"
fi
# Demonstrations (disposable images; transcripts kept).
mkdir -p "$WORK/demo"
python3 bench/demo.py "$WORK/normal" --qemu "$QEMU" --transcript "$WORK/demo/demo-transcript.txt"
for d in baseline injection speculative speculative-benign; do
  (cd "$WORK/normal" && python3 agent/agent.py --qemu "$QEMU" --log "$WORK/demo/agent-$d.jsonl" \
     demo $d > "$WORK/demo/agent-$d.txt" 2>&1)
done
echo "done: $WORK"
