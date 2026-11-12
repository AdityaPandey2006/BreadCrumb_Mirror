# Step 10: failure testing, measurements, graphs and demonstrations

Branch `step10`, 11 November 2026, existing x86 xv6-public tree. Step 10 adds
no new speculation feature. It adds failure tests, crash points in the output
release window, a measurement harness with raw data and figures, and final
demonstrations. Testing found **two kernel/tooling defects and one test race**;
all three are fixed with regression tests (section 3). The eight-block
configuration is unchanged (`NSHADOW 8`, `MAXOPBLOCKS 10`, `LOGSIZE 30`,
`NBUF 30`); no capacity upgrade (R04) was made.

Results are in `step10-verification.md`, the case-by-case matrix in
`step10-test-matrix.md`, raw evidence in `evidence/step10/`.

## 1. Reproducible environment (D49-D51)

* `bench/stage.sh normal|test|orig DIR [REV]` copies the source (working tree
  or a commit) into its own LF directory and builds it there:
  * `normal` - production kernel, `make -B xv6.img fs.img`;
  * `test` - fault-injection kernel, `make -B SHADOW_TEST=1 xv6.img fs.img`;
  * `orig` - **original xv6 baseline**: commit `fd7d454` (stock x86 xv6 plus
    the course `add`/`hello` syscalls) with `FSSIZE` raised 1000 -> 2000 to
    match the modified tree, plus the portable benchmark `spbench.c`
    compiled with `ORIG_XV6`.
  Each build gets `STAGE-INFO.txt`: source commit or `worktree@HEAD`, dirty
  flag, a SHA-256 of every staged source file, build command, image hashes,
  and the count of test-only kernel symbols (0 in normal/orig builds).
  Separate directories mean a build can never replace another build's images.
* The Windows checkout uses CRLF; staging converts text files to LF.
* `bench/env-report.sh` records date, commit, host CPU/memory, OS, gcc, ld,
  make, Python, QEMU version and path, acceleration (TCG), guest memory and
  kernel limits. The reference machine: AMD Ryzen 7 170 (16 host CPUs),
  7.4 GiB, WSL2 Ubuntu 26.04, gcc 15.2.0, binutils 2.46, QEMU 11.1.50 (TCG),
  Python 3.14.4. Earlier step evidence came from teammates' machines
  (gcc 11.4, QEMU 11.1.50); Step-10 evidence is all from the reference machine.
* Crash tests copy the images once per crash point and then **reboot the same
  `fs.img`**; nothing is rebuilt between crash and reboot.
* `bench/run-all.sh` runs everything: environment, staging, correctness
  matrix, measurements, figures and demonstrations.

## 2. Failure tests and crash expansion

### New test-only kernel hooks (SHADOW_TEST=1 only; D55)

| Hook | Where | Purpose |
|---|---|---|
| `ST_FAIL` value `STF_CONSOLE` (2) / `STF_PIPE` (3) | `shadow.c`, `console.c`, `pipe.c` | fail the next held-console / held-pipe page allocation (0 or 1 keep the old shadow-page meaning) |
| `ST_FREEPAGES` (14) -> `kfreepages()` | `kalloc.c` | count free physical pages; exact leak checks and memory measurements |
| crash point 5 `SPC_AFTER_DURABLE` | `proc.c` | filesystem durable, no held output released |
| crash point 6 `SPC_MID_PIPE` | `pipe.c` | first held pipe record delivered, rest not |
| crash point 7 `SPC_BEFORE_CONS` | `console.c` | pipe output released, console not yet |
| crash point 8 `SPC_MID_CONS` | `console.c` | first 16 held console bytes emitted, rest not |

The normal build contains none of these (`test_only_kernel_symbols=0`).

### New tests

* `spfailure.c` - F1 commit releasing 1500 held bytes into a 512-byte outside
  pipe whose reader prints between reads; F2 cancelled sleepers; F3 owner exit
  with held console and pipe output; F4 outside reader already blocked
  (commit and abort); F5 outside writer blocked on a speculative reader;
  F6 held write to a full outside pipe; F7 lifecycle error matrix; F8 held
  page allocation failure; F9 20 mixed commit/abort cycles with exact
  free-page accounting. Checks made inside a speculation are recorded
  silently and reported after completion, because console output inside a
  speculation is itself held (an earlier draft hid failures this way).
* `spfailure outcrash setup|N|verify` and harness suite `output-crash`.
* `tests/regress-root-hole.sh` (defect D1).
* Harness suites `failure`, `drainfull`, `output-crash`, a `--timeout` hang
  detector, and a host-side check for every `LEAK_*` token.
* `tests/run-matrix.sh` - the complete matrix, recording PASS/FAIL per run in
  `matrix.tsv` without stopping at the first failure.

### Filesystem recovery vs output delivery (D26, D39, D44 made concrete)

Crash points 1-4 (existing) show the atomic filesystem commit: old state before
the commit record, new state after it. Crash points 5-8 are all **after** the
durable point: recovery always yields the complete new filesystem, but held
output that had not yet been released is **lost**, and output being released
can be **partially delivered** (point 8: exactly the first 16 bytes reached
the serial port). Nothing replays lost output after reboot; speculation
contexts are memory-only. Filesystem durability therefore does **not** imply
exactly-once console or pipe delivery. The same holds if the releasing process
is killed during pipe delivery: delivery stops and the remainder is dropped.

## 3. Defects found and fixed

### D1 (kernel-visible, tooling root cause): directory entries lost on abort

* **Symptom.** After adding three user programs to `fs.img`, `spectest`
  failed "speculative overwrite discarded by xabort": a file created
  *publicly* before an unrelated speculation vanished after that
  speculation aborted. Unmodified HEAD with three dummy programs reproduces
  it (`evidence/step10/development/root-hole-found-HEAD-plus-3-programs.txt`).
* **Cause.** Stock `mkfs` sets the root size to `((off/BSIZE)+1)*BSIZE`. With
  exactly 32 root entries (a full block) that adds a whole block that was
  never allocated (`addrs[1] == 0`). The first lookup past 512 bytes calls
  `bmap`, which allocates the block **only in the cached inode**: xv6's
  `writei` calls `iupdate` only when the size grows, which it does not. Stock
  xv6 never notices because the root inode stays cached. Step 6's
  `inode_invalidate()` (run by abort) reloads cached inode fields from disk,
  so `addrs[1]` becomes 0 again and every entry in that block disappears; a
  later lookup allocates a fresh zero block.
* **Fix.** `mkfs.c` rounds up to the blocks actually allocated
  (`(off+BSIZE-1)/BSIZE*BSIZE`) and fails the build if the root size ever
  covers an unallocated block. Holes cannot arise otherwise (`writei`
  rejects `off > size`). The kernel's lazy `iupdate` is unchanged and is
  recorded as a limitation.
* **Regression.** `tests/regress-root-hole.sh` pads the root to a multiple of
  32 entries, shows the failure with the stock rounding restored, and
  passes with the repository `mkfs.c`.

### D2 (kernel): commit release deadlocked against outside readers

* **Symptom.** F1 hung on 1 and 3 CPUs
  (`development/drainfull-before-fix-cpu1.txt`).
* **Cause.** `spec_complete()` called `spec_commit_durable()` (pipe drain, then
  console drain) while still holding the coarse filesystem gate. When held
  pipe output exceeded the room in the outside pipe, the committer slept
  waiting for the reader; the reader's next ordinary console `printf` runs
  `begin_op()` -> `fs_enter()` and waited for the gate. Every filesystem
  operation in the system then stopped. This also contradicted D42, which
  states release happens without the gate.
* **Fix.** At the durable point the held pipe records and console page are
  only *detached* from the context (`spec_pipe_take`, `spec_console_take`).
  Cleanup, owner detach and `fs_leave()` follow; then `spec_release_held()`
  delivers pipe output and then console output (`spec_pipe_deliver`,
  `spec_console_emit`) with no gate, inode lock or spinlock held. Ordering
  guarantees are unchanged: filesystem durable -> pipe -> console (D37, D42,
  D48). `xcommit` still returns only after release completes, so a commit
  whose held pipe output exceeds the room in the pipe waits for the outside
  reader.
* **Regression.** `spfailure drainfull` (suite `drainfull`), passes on 1 and
  3 CPUs (`development/drainfull-after-fix-cpu*.txt`).

### D3 (test): spfiletest outsider race

* **Symptom.** The `shadow` suite failed intermittently at 1 CPU (1 of 3
  boots): "abort preserves the original public bytes".
* **Cause.** The outside process waited `sleep(2)` (about 20 ms) after the go
  signal and assumed the speculation had already copied blocks 0 and 2. One
  emulated speculative write can take about a tick, so the outsider's write
  sometimes landed *before* the copy and was (correctly) public.
* **Fix.** The outsider polls by rewriting one original byte of block 2 until
  the write is refused (D20). The probe changes nothing while it succeeds.
  25 consecutive runs at 1 CPU and 5 at 3 CPUs passed afterwards.

## 4. Measurements (D49-D54, D59)

* **Program.** `spbench.c` - one binary for all builds (`ORIG_XV6` removes the
  speculative modes). Timing uses `rdtsc` in user mode; each attempt records
  begin / operation / completion phases and the total. Every CSV line is one
  `write()` made while not speculative, so held output never contains
  measurements.
* **Calibration.** `spbench cal 300` spins 300 timer ticks; the host
  timestamps the two marker lines. Result on every boot: 0.313 ns per cycle
  (3.19 GHz guest TSC, following the host clock under TCG) and 10.0 ms per
  tick. Cycles are converted with the calibration of the *same boot*.
* **Configurations** (all 1 CPU, QEMU TCG, 512 MiB, serial to a pipe - not a
  terminal):
  * `orig` - original xv6 (FSSIZE 2000), ordinary mode only;
  * `ord` - modified kernel, ordinary process (pays the filesystem gate and
    the shadow-index checks, so it is **not** an original-xv6 baseline);
  * `commit`, `abort` - speculative attempt `xbegin`; operation; `xcommit` or
    `xabort`.
* **Workloads.** `getpid`; empty speculation; overwrite of 1-128 blocks
  (0.5-64 KiB) of an existing file; create + write 0-128 blocks; extend a
  1-block file by 1-16 blocks; unlink; console write 64-6144 B; pipe to an
  outside reader 64-4000 B; pipe inside the speculation 64-512 B.
  Concurrency: P = 1, 2, 4, 8 processes x 20 attempts each overwriting one
  block (own file each, or the same block), on 1, 2 and 4 CPUs.
  Memory: pages held by one live attempt (test build).
* **Samples.** 50 iterations per point x 5 independent boots (console: 20 x 5);
  iteration 0 of each boot is a cold-cache warm-up reported separately.
  Concurrency: 3 boots per CPU count. Figures show medians with interquartile
  ranges; `summary-micro.csv` lists n, failures and warm-up medians.
* **Failed attempts** (short writes at the cap) are kept, drawn with hollow
  markers labelled as failures, and never mixed into successful timings. The
  cliff is shown, not cropped.
* `bench/plot.py` regenerates every figure and summary table from the raw
  CSVs (`bench/requirements.txt` pins matplotlib 3.10.7 / numpy 2.5.4).

## 5. Demonstrations (D57)

* `specrun commit|abort|keep PATH STEP...` runs a command list inside one
  speculation (each step is one shell argument, words joined by commas:
  `rm,a`, `echo,hi,into,b`, `cat,f,pipe,wc`, `pause,300`). `keep PATH`
  commits only if PATH still exists in the private view.
* `bench/demo.py` - 20 checked demonstrations on a disposable image: ordinary
  effects; private changes invisible to a live outside observer; abort
  (handout case: `rm a`, `echo hi` into `b`, then abort); commit with output
  released after the commit; internal pipe; capacity limit; first-committer
  -wins conflict; outside-write refusal; and an unguarded commit showing that
  containment is not detection.
* Rule-based agent: `demo speculative` (the scripted injected deletion runs
  inside a guarded speculation and is discarded) and `demo speculative-benign`
  (a benign draft is committed and `APPROVED` released after the commit).
  No LLM or API dependency.

## 6. Decisions recorded (spreadsheet rows D49-D62)

D49 timing source; D50 QEMU TCG and CPU counts; D51 original-xv6 baseline;
D52 samples and statistics; D53 workloads and failed-attempt labelling;
D54 resource measurement in the test build only; D55 fault-injection
selector, free-page counter and crash points 5-8; D56 matplotlib, pinned;
D57 specrun + agent demonstration modes; D58 GDB session on defect D1
(`tests/gdb-root-hole.sh`, transcripts in `evidence/step10/gdb/`);
D59 keep the eight-block configuration;
D60 mkfs root-size fix (defect D1); D61 release held output after leaving the
filesystem gate (defect D2); D62 deterministic outsider probe (defect D3).

## 7. Files changed

Kernel: `proc.c` (detach at durable point, release after `fs_leave`, crash
point 5), `pipe.c` (`spec_pipe_take`/`spec_pipe_deliver`, crash point 6,
`STF_PIPE`), `console.c` (`spec_console_take`/`spec_console_emit`, crash
points 7-8, `STF_CONSOLE`), `shadow.c` (`ST_FAIL` selector, `ST_FREEPAGES`,
`shadow_test_fail`), `kalloc.c` (`kfreepages`, test build), `defs.h`,
`shadowtest.h`. Tools: `mkfs.c`. User programs: new `spfailure.c`,
`spbench.c`, `specrun.c`; `spfiletest.c` (D3). Build: `Makefile`. Host:
`tests/run-spec-tests.py`, new `tests/run-matrix.sh`,
`tests/regress-root-hole.sh`, `bench/*`, `agent/agent.py`,
`agent/test_agent.py`.
