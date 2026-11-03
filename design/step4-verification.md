# Step 4 verification — 3 November 2026

Run locally on the x86 xv6 tree with `~/qemu/build/qemu-system-i386` under
WSL Ubuntu (gcc 15.2). The Windows checkout uses CRLF line endings
(`core.autocrlf=true`), which breaks the Perl build scripts, so every build
ran in an LF copy of the working tree (`~/bc-step4`, synced from the
checkout; `design/` and `agent/` excluded). The harness boots disposable
copies of the images. Raw logs are under `design/evidence/step4/`;
`run-summary.txt` is the console output of the final matrix.

| Configuration / check | Result | Evidence |
|---|---|---|
| Test build `make -B SHADOW_TEST=1 xv6.img fs.img` | PASS | `build-shadowtest.txt` |
| Shadow suite (shadowtest + spfiletest, then spectest), 1 CPU | PASS | `shadow-cpu1.txt` |
| Shadow suite, 3 CPUs, five repeats | PASS (5/5 + spectest) | `shadow-cpu3-repeat5.txt` |
| File suite (spfiletest, incl. allocation failure + no-public-write assertion), 2 CPUs × 10 | PASS 10/10 | `files-cpu2-repeat10.txt` |
| File suite, 4 CPUs × 10 | PASS 10/10 (1090 PASS lines, 0 FAIL) | `files-cpu4-repeat10.txt` |
| Normal build `make -B xv6.img fs.img` | PASS | `build-normal.txt` |
| Normal kernel has no test-only symbols (`sys_shadowtest`, `shadow_test`, `shadow_note_public_write`) | 0 found | `run-summary.txt` |
| Normal lifecycle suite (spectest, spfiletest, addtest, hello_test, mytest), 1 CPU | PASS | `normal-lifecycle-cpu1.txt` |
| Normal lifecycle suite, 3 CPUs | PASS | `normal-lifecycle-cpu3.txt` |
| Normal file suite, 3 CPUs × 3 | PASS 3/3 | `normal-files-cpu3-repeat3.txt` |
| Full `usertests`, 1 CPU | ALL TESTS PASSED | `usertests-cpu1.txt` |
| Full `usertests`, 2 CPUs | ALL TESTS PASSED | `usertests-cpu2.txt` |
| Python agent tests `python3 -m unittest discover -s agent -v` | 15 passed | `agent-unittest.txt` |

Reproduce from an LF checkout (pass `--qemu` if QEMU is not at
`../build/qemu-system-i386`):

```sh
make -B SHADOW_TEST=1 xv6.img fs.img
python3 tests/run-spec-tests.py --cpus 1 --log logs/shadow-cpu1.txt
python3 tests/run-spec-tests.py --cpus 3 --repeat 5 --log logs/shadow-cpu3.txt
python3 tests/run-spec-tests.py --suite files --cpus 4 --repeat 10 --log logs/files-cpu4.txt

make -B xv6.img fs.img            # restore normal mode (no test syscall)
python3 tests/run-spec-tests.py --suite lifecycle --cpus 3 --log logs/lifecycle.txt
python3 tests/run-spec-tests.py --suite usertests --cpus 1 --log logs/usertests.txt
```

Write logs somewhere persistent: a WSL restart clears `/tmp` (this happened
once during development and the first copy of the logs was lost; the whole
matrix was then rerun into `design/evidence/step4/`). The WSL build copy was
left in normal mode.

The expected `usertests` uio protection fault still appears and the run ends
with `ALL TESTS PASSED`. `zombie!` lines come from init reaping orphaned
members (for example, the killed member in the owner-death tests). They are
not console buffering, which is unimplemented.

## Test sensitivity (deliberately broken kernels)

Each mutation was applied only to the WSL build copy, run with the file
suite, and then reverted (`development/`):

| Mutation | Outcome |
|---|---|
| Speculative write path disabled (writes go public) | `panic: log_write outside of trans` on the first speculative write |
| Outside-writer refusal (D20 block check) disabled | 7 FAILs, including "abort preserves the original public bytes" |
| Speculative read path disabled (reads public view) | 12 FAILs (own bytes, parent/child sharing, indirect block, partial view, ...) |
| `FSSIZE` back to 1000 with the new test binary | usertests `panic: balloc: out of blocks` (903/1000 blocks used) |

## Failures found and corrected during development

1. Full `usertests` panicked with `balloc: out of blocks` in the big-files
   test. Cause: the new 43 KB `_spfiletest` raised the image to 903 of 1000
   blocks; the test needs about 142 free. Fixed by raising `FSSIZE` to 2000.
   It was not a Step-4 code defect: the same panic reproduces at 1000 blocks
   (log above), and it passes at 2000.
2. A `-Werror` unused variable in the first spfiletest draft (build failure,
   fixed before any run).

The first complete spfiletest run already passed; no kernel logic defect
was found by the tests. That is why the mutation runs above were added, to
show the tests can fail. None of this replaces the handout's required GDB
session.

## Coverage boundaries

Verified: the Step-4 subset in `step4-implementation.md`: private reads and
non-extending writes, outsider isolation and D20 refusal, D21 descriptors,
D22 partial writes, abort/owner-death discard, rejected namespace and
extension operations, and no `log_write()` by speculative processes.

Not verified or not implemented: durable commit (nonempty `xcommit` still
returns -1), create/delete/extension isolation, console/pipe buffering,
snapshot reads, the final conflict policy, crash atomicity of speculation,
and performance measurements. The `ST_PUBWRITES` assertion covers
`log_write()` only. It is the single path to a dirty public buffer, a log
entry, block allocation or an inode update; direct `bwrite()` calls are made
only by log commit and recovery.
