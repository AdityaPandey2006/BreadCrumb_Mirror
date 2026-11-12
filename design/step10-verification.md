# Step 10 verification - 11 November 2026

All results below were produced on the reference machine (AMD Ryzen 7 170,
16 host CPUs, WSL2 Ubuntu 26.04, gcc 15.2.0, QEMU 11.1.50 TCG) from the
`step10` working tree (not yet committed; each build's exact source hash is in
its `STAGE-*.txt`). Raw logs and data are under `evidence/step10/`.
Nothing here is extrapolated: every number comes from a raw file listed next
to it.

## 1. Environment and builds

| Build | Command | Test-only kernel symbols | Record |
|---|---|---|---|
| normal (production) | `sh bench/stage.sh normal DIR` | 0 | `STAGE-normal-matrix.txt` |
| test (fault injection) | `sh bench/stage.sh test DIR` | 3 | `STAGE-test-matrix.txt` |
| orig (original xv6, FSSIZE 2000) | `sh bench/stage.sh orig DIR` | 0 | `STAGE-orig-matrix.txt` |

The matrix, micro-benchmarks, memory measurement and demonstrations used
these builds (`run-all-console.txt`). The concurrency re-run used rebuilt
`normal`/`orig` images (`results/STAGE-normal.txt`, `STAGE-orig.txt`) whose only
guest-code difference is the per-worker byte pattern in `spbench conc`.
After these runs only host-side files changed (`bench/plot.py` figure layout,
documentation, `tests/gdb-root-hole.sh`); no kernel or test-program source
changed.

Environment: `evidence/step10/environment.txt`. Calibration (`results/cal.csv`,
20 measurements over 10 boots): 0.31303-0.31309 ns per guest TSC cycle,
9.998-10.000 ms per timer tick.

## 2. Correctness matrix (`tests/run-matrix.sh`, `matrix/matrix.tsv`)

**37 runs, 37 PASS, 0 FAIL.** Repeat = consecutive runs of the suite in one boot.

| Suite | Build | CPUs (repeats) | Result |
|---|---|---|---|
| shadow (shadowtest, spfiletest, spcommit, spconsoletest, sppipetest, spconflict, spectest) | test | 1, 2, 3, 4 (x3 each) | PASS |
| conflict | test | 1, 3 (x9 each) | PASS |
| failure (spfailure F1-F9) | test | 1, 2, 3, 4 (x3 each) | PASS |
| namespace | test | 1, 4 (x3) | PASS |
| console / pipe | test | 1, 3 (x3) | PASS |
| crash (points 1-4) / namespace-crash (1-4) | test | 1, 3 | PASS |
| output-crash (points 5-8) | test | 1, 3 (x3) | PASS |
| lifecycle | test; normal | 1; 1, 3 | PASS |
| failure (F1-F7) | normal | 1, 3 (x3) | PASS |
| drainfull (defect D2 regression) | normal | 1, 3 (x3) | PASS |
| namespace / conflict | normal | 3 (x3) | PASS |
| usertests | test 1 CPU; normal 1 and 2 CPUs; orig 1 CPU | - | ALL TESTS PASSED (4/4) |
| root-hole regression (defect D1) | test | 1 | PASS (stock mkfs fails as expected, fixed mkfs passes) |
| agent unit tests | host | - | 18 tests OK |

Selected outcomes from the raw logs:

* F2 cancelled sleepers: owner killed while members were in `sleep(1000)` and
  in a blocked internal-pipe read; all participants were gone **1-3 ticks**
  after the kill in all 18 runs (bound checked: < 300).
* F9 resource return: free pages **56704 before and 56704 after** 20 mixed
  commit/abort cycles in all 12 test-build runs; shadow pages 0.
* F1 drain: the outside reader received all **1500** bytes through the
  512-byte pipe and kept doing filesystem work.
* Leak tokens: no `LEAK_*` token appeared in any serial log.

### Output-release crash points (`output-crash`, 6 runs per point)

| Point | Crash moment | Recovered filesystem | Console bytes before halt | Pipe bytes seen by the outside reader before halt |
|---|---|---|---|---|
| 1-4 | (existing) before/after the commit record | old at 1-2, new at 3-4 | - | - |
| 5 | durable, nothing released | complete new (6/6) | none (6/6) | none (6/6) |
| 6 | first held pipe record delivered | complete new (6/6) | none (6/6) | none observed (6/6) |
| 7 | pipe released, console not yet | complete new (6/6) | none (6/6) | none observed (6/6) |
| 8 | first 16 console bytes emitted | complete new (6/6) | exactly the 16-byte prefix (6/6) | first record printed in 1/6 (3 CPUs), none in 5/6 |

After every reboot the held output was absent (not replayed) and a new
1500-byte file could be created and removed (allocator usable). Conclusion:
the filesystem commit is atomic and durable, but console/pipe delivery after
the durable point can be lost or partial; filesystem durability does not
imply exactly-once output.

## 3. Defects found and their regression evidence

| Defect | Before fix | After fix |
|---|---|---|
| D1 root-directory hole (mkfs) | `development/root-hole-found-HEAD-plus-3-programs.txt` (unmodified kernel), `development/root-hole-diagnosis-entries-lost-after-abort.txt`, `gdb/gdb-stock-mkfs.txt` | `matrix/root-hole-regression.txt`, `gdb/gdb-fixed-mkfs.txt`, all matrix runs |
| D2 release deadlock under the filesystem gate | `development/drainfull-before-fix-cpu1.txt` (hang) | `development/drainfull-after-fix-cpu{1,3}.txt`, `matrix/drainfull-*` |
| D3 spfiletest outsider race | `development/spfiletest-race-before-fix-*.txt` (1 of 3 boots) | 25 + 5 consecutive runs (development), `matrix/shadow-*` |

### GDB session (handout requirement; `gdb/`)

`tests/gdb-root-hole.sh` starts QEMU halted with a gdb stub, breaks in
`bmap()` whenever root block 1 is unallocated, and logs every `fs_finish()`:

* **stock mkfs** (root size 1024, one allocated block): hit 1 at boot - init's
  lookup of `console` reads offset 512 and `bmap` fills the hole in the
  cached inode only (`readi` <- `dirlookup`). After an aborted speculation
  (`fs_finish(committed=0)` -> `inode_invalidate`) the reloaded root again
  has `addrs[1]=0` (hits 2 and 3, during `create`/lookup of `specleak`), so the
  entries written into the first copy are gone; spectest reports `FAIL:
  speculative overwrite discarded by xabort`.
* **fixed mkfs** (root size 512): the only hit is a normal extension through
  `writei` <- `dirlink` (which calls `iupdate`), no hit after any abort, and
  `SPEC TESTS PASSED`.

## 4. Measurements (`results/`, figures in `figures/`)

1 CPU, QEMU TCG, NSHADOW=8, LOGSIZE=30, NBUF=30. Medians with
interquartile range [q1-q3] over n = 245 iterations (49 per boot x 5 boots,
warm-up excluded; console n = 95). `FAIL` = every attempt was short at the
cap; the bytes actually written are given. Full table: `figures/summary-micro.csv`.

| Workload | original xv6 | modified, ordinary | speculative + commit | speculative + abort |
|---|---|---|---|---|
| getpid | 0.010 ms | 0.010 ms | - | - |
| begin + empty completion | - | - | 0.099 ms [0.098-0.102] | 0.091 ms [0.090-0.094] |
| overwrite 1 block | 1.28 [1.22-1.37] | 1.40 [1.34-1.48] | 2.03 [1.94-2.15] | 0.89 [0.88-0.92] |
| overwrite 4 blocks (2 KiB) | 3.26 | 3.42 | 4.16 | 1.36 |
| overwrite 8 blocks (4 KiB, cap) | 5.69 [5.46-5.95] | 5.92 [5.68-6.11] | 6.93 [6.69-7.21] | 1.97 [1.91-2.09] |
| overwrite 9 blocks | 6.07 | 6.37 | FAIL, 4096 B written | FAIL, 4096 B written |
| overwrite 128 blocks (64 KiB) | 105.7 | 111.3 | FAIL, 4096 B written (6.96) | FAIL, 4096 B (2.05) |
| create empty file | 2.59 | 3.48 | 5.49 | 3.57 |
| create + 5 blocks (cap) | 8.22 | 9.37 | 10.93 | 5.81 |
| create + 6 blocks | 8.83 | 9.99 | FAIL, 2560 B written | FAIL, 2560 B |
| extend by 6 blocks (cap) | 6.60 | 7.12 | 8.44 | 3.53 |
| extend by 7 blocks | 8.49 | 9.22 | FAIL, 3072 B written | FAIL, 3072 B |
| unlink | 2.25 | 2.43 | 3.59 | 1.33 |
| console 64 B | 1.05 | 1.07 | 1.22 | 0.23 |
| console 4096 B | 64.7 | 64.5 | 64.2 | 0.24 |
| console 6144 B | 96.7 | 97.8 | FAIL, 4096 B held (64.8) | FAIL, 4096 B (0.24) |
| outside pipe 64 B | 0.030 | 0.035 | 0.248 | 0.168 |
| outside pipe 4000 B | 0.80 | 0.89 | 5.87 | 0.18 |
| internal pipe 512 B | 0.066 | 0.079 | 0.188 | 0.178 |

(times in ms)

Phase breakdown (median, overwrite, commit): 1 block - begin 0.015, writes
0.80, `xcommit` 1.19 ms; 8 blocks - 0.016, 1.79, 5.07 ms. Abort of 8 blocks:
`xabort` 0.16 ms. Memory (`results/res.csv`, test build): one page per shadow
entry (1-8), a create of N blocks holds N+3 entries (inode, bitmap, directory),
an extension N+2; one page per held console or outside-pipe stream; 0 pages
not returned after abort in every case.

Interpretation (and limits of the interpretation):

* **Ordinary execution on the modified kernel** costs 0-9 % more than original
  xv6 for overwrites and 5-34 % more for metadata-heavy operations (empty
  create 3.48 vs 2.59 ms), from the filesystem gate, view checks and shadow-
  index lookups. getpid is unchanged.
* **Speculative commit** of k blocks costs ordinary-write + roughly a fixed
  0.6-1.0 ms: the speculation's own exclusive log transaction (all blocks
  logged once, header, install, header clear). Relative overhead falls from
  1.45x (1 block) to 1.17x (8 blocks).
* **Abort** is cheaper than doing the write ordinarily (no disk writes at all):
  0.89 ms vs 1.40 ms for one block, 1.97 vs 5.92 ms for eight.
* **The cliff**: past 8 blocks (overwrite), 5 data blocks (create) or 6 data
  blocks (extend) every speculative attempt is a short write; its time stays
  flat because only the capped prefix is written. These are failed attempts,
  plotted as such.
* **Held output**: console cost is dominated by emulated UART output, which is
  paid at release; aborting discards it for 0.24 ms. Outside-pipe output is
  much more expensive to commit (5.87 vs 0.89 ms at 4000 B) because the
  releasing committer waits for the outside reader to drain bytes beyond the
  512-byte pipe (defect D2 fix keeps this wait outside the filesystem gate).
* These are emulator timings on one machine; ratios between configurations
  are the meaningful result, not absolute milliseconds.

### Concurrency (`results/conc.csv`, `figures/summary-conc.csv`)

P processes x 20 attempts, each overwriting one 512-byte block; 3 boots per
CPU count; medians of successful writes per second (figure 6, 7).

| | 1 CPU, P=1 -> P=8 | 2 CPUs, P=1 -> P=8 | 4 CPUs, P=1 -> P=8 |
|---|---|---|---|
| original xv6, own file | 471 -> 659 | 465 -> 627 | 485 -> 665 |
| modified, ordinary, own file | 343 -> 303 | 372 -> 342 | 384 -> 352 |
| speculative commit, own file | 211 -> 200 | 215 -> 202 | 221 -> 203 |
| speculative commit, same block | 218 -> 206 | 223 -> 211 | 224 -> 197 |

* Original xv6 gains from overlapping processes (log group commit); the
  modified kernel does not, in ordinary or speculative mode, at any CPU
  count: the coarse filesystem gate serializes all filesystem work. This is
  the measured cost of D28/D47.
* Conflicts (same block, speculative): **14 of 2700 attempts**; median 0 % for
  P <= 4, 1.2 % (1 and 2 CPUs) and 0.6 % (4 CPUs) at P = 8. Own-file runs: 0.
  Conflicts are rare because each attempt is short and the gate serializes
  most of it.
* **Invalid first run, kept for the record**: the first concurrency run wrote
  the same bytes the file already held, and content-based validation then
  never sees a change - 0 conflicts in every configuration. This is a real
  property of first-committer-wins as implemented (an identical rewrite is
  not a conflict), but it made that data set meaningless; it is preserved as
  `results/conc-identical-bytes-invalid.csv` and not plotted. The run above
  uses distinct bytes per worker and attempt (`spbench conc`, rebuilt
  binaries recorded in `results/STAGE-normal.txt` / `STAGE-orig.txt`; the
  micro-benchmark builds are `STAGE-*-micro.txt`).

## 5. Demonstrations (`demo/`)

`bench/demo.py` on the production build: **20/20 checks passed**
(`demo/demo-transcript.txt`, serial log alongside). Rule-based agent:
`demo baseline` and `demo injection` show the effects escaping (as before);
`demo speculative` contains and discards the injected deletion and the
`APPROVED` message; `demo speculative-benign` commits the draft and releases
`APPROVED` only after the commit (`demo/agent-*.txt`, `.jsonl`).

## 6. What was not run or not verified

* No KVM or real-hardware runs; all timings are QEMU TCG on one machine.
* Concurrency measured at 1, 2 and 4 CPUs only; micro-benchmarks at 1 CPU only.
* `usertests` was run once per configuration (4 runs), not repeated.
* The crash suites inject crashes at named points with `panic`; torn sector
  writes and disk write caches are not modelled.
* Pipe delivery at crash points 6-8 is observed, not asserted.
* No larger shadow/log/cache configuration (R04) was built or measured.
* Earlier steps' mutation tests (Step 5) were not re-run in Step 10.
