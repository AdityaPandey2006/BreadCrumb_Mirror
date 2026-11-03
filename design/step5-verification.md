# Step 5 verification — 3 November 2026

Same environment as Step 4: WSL Ubuntu, gcc 15.2,
`~/qemu/build/qemu-system-i386`. Builds run in an LF copy of the checkout
(the Windows tree uses CRLF). Raw logs are in `design/evidence/step5/`.
`run-summary.txt` is the console output of the first full matrix, and
`run-summary-2.txt` is the console output of the rerun after the fixes
described below.

## Results

| Configuration / check | Result | Evidence |
|---|---|---|
| Baseline before Step 5: Step-4 shadow suite, 1 CPU | PASS | `development/baseline-step4-shadow-cpu1.txt` |
| Test build `make -B SHADOW_TEST=1 xv6.img fs.img` | PASS | `build-shadowtest.txt` |
| Shadow suite (shadowtest, spfiletest, spcommit, then spectest), 1 CPU | PASS | `shadow-cpu1.txt` (rerun after the `#ifdef` fix) |
| Shadow suite, 3 CPUs × 3 | PASS 3/3 | `shadow-cpu3-repeat3.txt` |
| Commit suite (spcommit, 74 checks per run), 2 CPUs × 5 | PASS 5/5 | `commit-cpu2-repeat5.txt` |
| Commit suite, 4 CPUs × 5 | PASS 5/5 (370 PASS, 0 FAIL) | `commit-cpu4-repeat5.txt` |
| Step-4 file suite, 4 CPUs × 3 | PASS 3/3 | `files-cpu4-repeat3.txt` |
| Crash/recovery suite, 1 CPU | 4/4 points PASS | `crash-cpu1.txt` |
| Crash/recovery suite, 3 CPUs × 2 | 8/8 PASS | `crash-cpu3-repeat2.txt` |
| Normal build `make -B xv6.img fs.img`; no test-only symbols | PASS; 0 symbols | `build-normal.txt`, `run-summary-2.txt` |
| Normal lifecycle (spectest, spfiletest, spcommit, addtest, hello_test, mytest), 1 and 3 CPUs | PASS | `normal-lifecycle-cpu1.txt`, `normal-lifecycle-cpu3.txt` |
| Normal commit suite (58 checks per run), 3 CPUs × 3 | PASS 3/3 (174 PASS) | `normal-commit-cpu3-repeat3.txt` |
| Full `usertests`, 1 and 2 CPUs | ALL TESTS PASSED | `usertests-cpu1.txt`, `usertests-cpu2.txt` |
| Python agent tests | 15 passed | `agent-unittest.txt` |

### Crash recovery (same `fs.img` rebooted, no rebuild or copy)

Each run creates `cra` (2 blocks) and `crb` (2 blocks). It then commits a
speculation changing 3 blocks across both files, halts at the crash point,
reboots the same image copy, and checks every block plus the sizes. It also
creates and deletes a file to show the filesystem is still usable.

| Point | Meaning | Expected | Recovered (`cra` / `crb` blocks) | Caller got a reply |
|---|---|---|---|---|
| 1 | before any log block | old | `OO` / `OO` | no |
| 2 | log blocks written, commit record not | old | `OO` / `OO` | no |
| 3 | commit record written, nothing installed | new | `NN` / `ON`* | no |
| 4 | 1 of 3 blocks installed home | new | `NN` / `ON`* | no |

\* `crb` block 0 was deliberately not changed by the speculation.

All three runs of every point (1 CPU, and 3 CPUs twice) gave these results.
"Caller got a reply: no" means the guest halted before `xcommit` returned;
filesystem durability is shown by the recovered state, not by a reply.

### Test sensitivity (deliberately broken kernels; WSL build copy only)

| Mutation | Outcome | Log (`development/`) |
|---|---|---|
| Original exclusive-flag bug restored (boolean flag, cleared by any commit, never re-set) | caught on the first run at 1 CPU and at 3 CPUs (stray speculative `log_write`) | `mutation-exclusive-cleared-cpu{1,3}-repeat5.txt` |
| Inode locks released before installing | live reader saw a partial/undurable view | `mutation-no-inode-locks-during-publish.txt` |
| D24 validation disabled | 4 FAILs (second committer overwrote the first) | `mutation-no-conflict-validation.txt` |
| D23 last kill check disabled | killed-before-point-of-no-return commit published | `mutation-no-kill-check.txt` |

## Failures found during development (recorded honestly)

1. **Kernel bug: the exclusive flag was cleared by another group's
   commit.** `begin_op_exclusive()` set a boolean flag and waited for the
   log to drain. `end_op()` cleared the flag after *any* commit, including a
   group commit that ended while the publisher was still waiting. Ordinary
   operations could then join the publication transaction. Isolation broke
   and the publisher's own writes counted as stray speculative
   `log_write()`s. It was caught intermittently (1 of about 8 runs) by the
   test-build counter (`development/bug-exclusive-cleared-by-group-commit.txt`).
   The cause was confirmed by tracing; it disappeared under extra tracing
   because of timing. **Fix:** states NONE/WAITING/OWNED, with only the
   OWNED transaction's own commit releasing it, plus an all-builds invariant
   panic if an OWNED transaction is shared. **Regression test:** spcommit's
   "commits interleaved with a busy ordinary writer". The restored bug is
   caught on the first run (table above). This bug is a good candidate for
   the handout's GDB session; no GDB session has been recorded yet.
2. **Test-program bugs (not kernel):** a byte array reused across a helper
   that also wrote into it. It first broke the reuse check and then made
   crash point 3 look "mixed", because the data being committed had been
   overwritten before it was written. Fixed with a dedicated buffer.
   `development/dev-crash-cpu1.txt` is the passing run after the fix.
3. **Unfaithful first mutation:** the first attempt to restore the bug
   (`run-summary.txt`, M1) left the publisher re-setting the flag after
   waiting, so the bug was not reproduced and the tests passed. It was
   replaced by the faithful mutation above.
4. **Normal build initially failed** (`-Werror` unused test-only helpers in
   `spcommit.c`). The first matrix's normal-build rows in
   `run-summary.txt` therefore ran against stale test-build images and are
   **invalid**. After guarding the helpers with `#ifdef SHADOW_TEST`, the
   normal build and its suites were rerun (`run-summary-2.txt`), and the
   1-CPU shadow suite was rerun in the test build. The script now stops on
   any build failure.

## Reproduce

```sh
make -B SHADOW_TEST=1 xv6.img fs.img
python3 tests/run-spec-tests.py --cpus 3 --repeat 3 --log logs/shadow.txt
python3 tests/run-spec-tests.py --suite commit --cpus 4 --repeat 5 --log logs/commit.txt
python3 tests/run-spec-tests.py --suite crash --cpus 3 --log logs/crash.txt
make -B xv6.img fs.img
python3 tests/run-spec-tests.py --suite lifecycle --cpus 3 --log logs/lifecycle.txt
python3 tests/run-spec-tests.py --suite usertests --cpus 1 --log logs/usertests.txt
```

Pass `--qemu` if QEMU is not at `../build/qemu-system-i386`. Keep logs out
of WSL `/tmp`, which is cleared when the distro restarts.

## Coverage boundaries

Verified: abort, durable single-transaction commit of the Step-4 subset,
live-reader all-old/all-new isolation, D23/D24/D25 behaviour, D20 during
and after commit, member changes surviving member exit, completion
permissions, reuse with no page leaks, commits under concurrent ordinary
writes, and crash recovery at four points.

Not verified or not implemented: extension/create/delete (Step 6);
console/pipe buffering and output release at `spec_commit_durable()`
(Steps 7-8); crashes during *ordinary* (non-speculative) transactions,
which are unchanged xv6; real disk write caches (QEMU raw image); and
performance.
