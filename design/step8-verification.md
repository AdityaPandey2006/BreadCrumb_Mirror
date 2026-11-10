# Step 8 Verification - 10 November 2026

Branch: `step7and8`. All guest test runs execute in QEMU against disposable disk image copies.
Host-side observer assertions in `tests/run-spec-tests.py` verify that secret strings discarded during abort or owner kill never appear in pipe outputs or console logs, and that committed external pipe messages appear strictly upon durable commit.

## Verification Summary

| Check | Suite / Command | CPUs | Result | Evidence File under `design/evidence/step8/` |
|---|---|---|---|---|
| Instrumented kernel & fs build | `make -B SHADOW_TEST=1 xv6.img fs.img` | - | PASS | `build-test.txt` |
| Pipe isolation & release (1 CPU) | `run-spec-tests.py --suite pipe --cpus 1` | 1 | PASS | `pipe-cpu1.txt` |
| Pipe isolation & release (3 CPUs, 3 repeats) | `run-spec-tests.py --suite pipe --cpus 3 --repeat 3` | 3 | PASS | `pipe-cpu3-repeat3.txt` |
| Console isolation & release (1 CPU) | `run-spec-tests.py --suite console --cpus 1` | 1 | PASS | `console-cpu1.txt` |
| Console isolation & release (3 CPUs, 3 repeats) | `run-spec-tests.py --suite console --cpus 3 --repeat 3` | 3 | PASS | `console-cpu3-repeat3.txt` |
| Shadow suite (shadowtest, spfiletest, spcommit, spconsoletest, sppipetest, spectest) (1 CPU) | `run-spec-tests.py --suite shadow --cpus 1` | 1 | PASS | `shadow-cpu1.txt` |
| Full shadow regression (3 CPUs, 2 repeats) | `run-spec-tests.py --suite shadow --cpus 3 --repeat 2` | 3 | PASS | `shadow-cpu3.txt` |
| Namespace regression (3 CPUs, 2 repeats) | `run-spec-tests.py --suite namespace --cpus 3 --repeat 2` | 3 | PASS | `namespace-cpu3.txt` |
| Filesystem commit crash points 1-4 (3 CPUs) | `run-spec-tests.py --suite crash --cpus 3` | 3 | PASS | `crash-cpu3.txt` |
| Namespace commit crash points 1-4 (3 CPUs) | `run-spec-tests.py --suite namespace-crash --cpus 3` | 3 | PASS | `namespace-crash-cpu3.txt` |
| Standard production kernel & fs build | `make -B xv6.img fs.img` | - | PASS | `build-normal.txt` |
| Standard xv6 usertests | `run-spec-tests.py --suite usertests --cpus 1` | 1 | ALL TESTS PASSED | `usertests-cpu1.txt` |
| Host Python agent regression | `python3 -m unittest discover -s agent` | - | 15 tests passed | `agent-unittest.txt` |

## Verified Scenarios in `sppipetest.c`

1. **Internal Pipeline Progress**: Parent and child sharing the same speculation context communicate across an internal pipe before commit. Child sends message to parent, parent reads it, both participate in speculation without hanging or deadlocking.
2. **External Pipe Commit Release**: Speculative write to an external pipe (`spec == 0` reader) is staged in memory; host observer and reader confirm zero bytes arrive before commit. Upon `xcommit()`, the staged bytes are drained and read by the external reader as `COMMITTED: SPEC_PIPE_COMMIT_MSG`.
3. **External Pipe Abort Discard**: Speculative process writes `LEAK_ABORT_PIPE_SECRET_777` to an external pipe and aborts via `xabort()`. Reader receives EOF (0 bytes); host observer verifies the secret string never leaked.
4. **Owner Kill Discard**: Speculative child owner writes `LEAK_KILL_PIPE_SECRET_666` to an external pipe and is killed. Pipe buffer is discarded; external reader observes EOF without leakage.
5. **Child Writer & Owner Commit Ordering**: Speculative child writes to an external pipe, child exits, owner commits; external reader receives both child's and owner's staged writes in exact chronological order: `CHILD_EXTERNAL_PIPE_MSG-OWNER_EXTERNAL_PIPE_MSG`.
6. **External Pipe Capacity & Short Writes**:
   - Speculative process writes 5000 bytes to an external pipe.
   - Initial 4000 bytes fill capacity to 4096 (accounting for record framing overhead).
   - Remaining capacity returns a short write count (88 bytes).
   - Subsequent write to full buffer returns `-1`.
   - Program aborts; all staged bytes are discarded cleanly with zero leaks.
7. **Broken Pipe Handling**: External reader closes read end before commit. Speculative drain detects broken pipe (`readopen == 0`), avoids panic, discards remaining bytes, and cleans up safely.
8. **Ordinary Pipes Retained**: Non-speculative processes create, write, and read ordinary 512-byte pipes with standard blocking, wakeups, and EOF semantics.

