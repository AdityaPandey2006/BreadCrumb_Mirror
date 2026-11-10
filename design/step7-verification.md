# Step 7 Verification - 10 November 2026

Branch: `step7`. All guest test runs execute in QEMU against disposable disk image copies.
Host-side observer assertions in `tests/run-spec-tests.py` verify that discarded strings never appear anywhere in the serial console log, and that committed messages appear strictly upon commit.

## Verification Summary

| Check | Suite / Command | CPUs | Result | Evidence File under `design/evidence/step7/` |
|---|---|---|---|---|
| Instrumented kernel & fs build | `make -B SHADOW_TEST=1 xv6.img fs.img` | - | PASS | `build-test.txt` |
| Console isolation & release (1 CPU) | `run-spec-tests.py --suite console --cpus 1` | 1 | PASS | `console-cpu1.txt` |
| Console isolation & release (3 CPUs, 3 repeats) | `run-spec-tests.py --suite console --cpus 3 --repeat 3` | 3 | PASS | `console-cpu3-repeat3.txt` |
| Full shadow regression (including console) | `run-spec-tests.py --suite shadow --cpus 3 --repeat 2` | 3 | PASS | `shadow-cpu3.txt` |
| Namespace regression | `run-spec-tests.py --suite namespace --cpus 3 --repeat 2` | 3 | PASS | `namespace-cpu3.txt` |
| Filesystem commit crash points 1-4 | `run-spec-tests.py --suite crash --cpus 3` | 3 | PASS | `crash-cpu3.txt` |
| Namespace commit crash points 1-4 | `run-spec-tests.py --suite namespace-crash --cpus 3` | 3 | PASS | `namespace-crash-cpu3.txt` |
| Standard kernel & fs build | `make -B xv6.img fs.img` | - | PASS | `build-normal.txt` |
| Standard xv6 usertests | `run-spec-tests.py --suite usertests --cpus 1` | 1 | ALL TESTS PASSED | `usertests-cpu1.txt` |
| Host Python agent regression | `python3 -m unittest discover -s agent` | - | 15 tests passed | `agent-unittest.txt` |

## Verified Scenarios in `spconsoletest.c`

1. **Speculative Isolation & Commit**: Console writes to fds 1 and 2 remain invisible until `xcommit()`, then appear together.
2. **Abort Discard**: `xabort()` discards secret string `LEAK_ABORT_SECRET_999`; host observer confirms zero leakage in serial output.
3. **Owner Death / Cancellation Discard**: Child owner writes `LEAK_KILL_SECRET_888` and is killed; host observer confirms zero leakage in serial output.
4. **Parent-Child Buffer Sharing**: Parent writes `PARENT_START-`, child writes `CHILD_PART-`, child exits, parent writes `PARENT_END\n` and commits; output arrives in exact sequential order `PARENT_START-CHILD_PART-PARENT_END\n`.
5. **Duplicated & Opened Descriptors**: Writes via `dup(1)` and newly opened `console` are intercepted and emitted on commit as `DUP_WRITE_MSG-OPEN_WRITE_MSG\n`.
6. **Empty Commit**: Speculation with zero filesystem mutations commits and successfully flushes `EMPTY_COMMIT_OUTPUT\n`.
7. **Buffer Capacity Overflow & Short Writes**:
   - Buffer fills up to 4000 bytes.
   - Next 150-byte write short-writes exactly 96 bytes (filling capacity to 4096).
   - Subsequent write to full buffer returns `-1`.
   - `xabort()` discards all 4096 bytes with zero leakage to physical console.
8. **Durable File Ordering**: Speculative file creation and write precedes console release; on `xcommit()`, disk contents are verified present and intact before post-commit execution completes.
9. **Non-Speculative Console Output**: Continues to operate without buffering or interference across all test suites and usertests.

