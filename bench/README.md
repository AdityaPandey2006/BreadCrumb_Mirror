# Step 10 tooling: builds, correctness matrix, measurements, figures, demos

Everything runs inside WSL/Linux from the repository root. QEMU is expected
at `~/qemu/build/qemu-system-i386` (pass another path as the first argument).

## One command

```sh
python3 -m venv ~/bc10/final/venv
~/bc10/final/venv/bin/pip install -r bench/requirements.txt   # matplotlib for figures
sh bench/run-all.sh ~/qemu/build/qemu-system-i386 ~/bc10/final [COMMIT]
```

About 1.5 hours on the reference machine. Without `COMMIT` the current working
tree is built (its exact content hash is recorded in each `STAGE-INFO.txt`).

## Step by step

```sh
Q=~/qemu/build/qemu-system-i386; W=~/bc10/final
sh bench/env-report.sh $Q > $W/environment.txt          # toolchain, QEMU, CPU, memory, flags
sh bench/stage.sh normal $W/normal                      # production build
sh bench/stage.sh test   $W/test                        # SHADOW_TEST=1 fault-injection build
sh bench/stage.sh orig   $W/orig                        # original xv6 baseline (fd7d454, FSSIZE 2000)

# correctness (writes $W/matrix/matrix.tsv + one raw log per run)
sh tests/run-matrix.sh $Q $W/normal $W/test $W/orig $W/matrix

# single suites, e.g.
(cd $W/test && python3 tests/run-spec-tests.py --qemu $Q --suite output-crash --cpus 3 --log /tmp/oc.txt)
(cd $W/test && python3 tests/run-spec-tests.py --qemu $Q --suite failure --cpus 1 --log /tmp/f.txt)
sh tests/regress-root-hole.sh $Q /tmp/roothole /tmp/roothole-logs

# measurements (raw CSV + serial logs in $W/results)
python3 bench/run-bench.py --qemu $Q --orig $W/orig --normal $W/normal --test $W/test --out $W/results
# figures + summary tables from the raw CSV
$W/venv/bin/python bench/plot.py --res $W/results --out $W/figures

# demonstrations (disposable image copies; transcript + serial log)
python3 bench/demo.py $W/normal --qemu $Q --transcript $W/demo/demo-transcript.txt
(cd $W/normal && python3 agent/agent.py --qemu $Q demo speculative)
(cd $W/normal && python3 agent/agent.py --qemu $Q demo speculative-benign)
```

The committed copies of the evidence produced this way are under
`design/evidence/step10/` (matrix, results, figures, demo, development).

## Files

| File | Purpose |
|---|---|
| `stage.sh` | stage + build one configuration in its own LF directory |
| `env-report.sh` | record the environment |
| `run-all.sh` | the whole Step-10 pipeline |
| `guest.py` | boot a disposable image copy and drive the serial console |
| `run-bench.py` | collect `micro.csv`, `cal.csv`, `conc.csv`, `res.csv` and serial logs |
| `plot.py` | regenerate all figures and summary tables |
| `demo.py` | the 20 checked demonstrations |
| `add-decisions.py` | one-off: wrote decisions D49-D62 into the CSV and spreadsheet |
| `requirements.txt` | pinned plotting dependencies |

Guest programs: `spbench` (measurements), `specrun` (demo utility),
`spfailure` (failure tests).
