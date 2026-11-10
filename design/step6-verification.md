# Step 6 verification - 10 November 2026

Branch: `step6-filesystem-isolation`. All guest runs use disposable disk copies.
Crash verification reboots the same modified copy without rebuilding fs.img.

| Check | Result | Raw evidence under evidence/step6/ |
|---|---|---|
| Forced instrumented kernel/filesystem build | PASS | build-test.txt |
| Forced normal kernel/filesystem build | PASS | build-normal.txt |
| Namespace suite, four CPUs, three consecutive runs | PASS | namespace-final-cpu4.txt |
| Instrumented shadow/file/commit/lifecycle regression, three CPUs | PASS | shadow-final-cpu3.txt |
| Namespace crash points 1-4, three CPUs | PASS: old before header, new after header | namespace-crash-final-cpu3.txt |
| Full xv6 usertests, one CPU, instrumented build | ALL TESTS PASSED | usertests-final-cpu1.txt |
| Python agent regression | 15 tests passed | agent-unittest.txt |
| Normal namespace suite, three CPUs | PASS | normal-namespace-cpu3.txt |
| Normal lifecycle/file/commit/example regression, three CPUs | PASS | normal-lifecycle-cpu3.txt |

Earlier development runs are retained in the same directory. The final namespace
suite includes the last-close capacity regression. An earlier file regression
exposed an over-broad savepoint on non-creating open: it consumed the allocation
failure trigger prematurely. Savepoints are now restricted to namespace
mutations and the existing partial-write tests pass.

The expected usertests uio trap is followed by `uio test done` and
`ALL TESTS PASSED`. `zombie!` can be printed by init while reaping descendants.

The final source disk images are normal builds without the conditional raw test
syscall. Use `make -B SHADOW_TEST=1 xv6.img fs.img` for namespace-crash or raw
shadow tests; force rebuild when switching flags. No inode disk format, shadow
limit or log/cache limit was changed.

## Scope demonstrated

Ordinary syscalls demonstrate private/public names, sizes and data; create/unlink
commit and abort; direct-to-indirect extension; directories and cwd fallback;
hard links; new handle aliases; open-unlinked public handles; full-cap namespace
rollback; consistent partial extension; allocation failure; owner cancellation;
and abort-only final-close cleanup.

Original-byte conflict validation covers metadata and data together. Every
changed block is published in one exclusive transaction under the filesystem
gate. Crash checks validate namespace and contents and then allocate a file to
check allocator usability.

## Limits

Console/pipe output remains unbuffered, uncopied reads are not snapshots, and
no exactly-once external output is claimed. The gate serializes filesystem
operations, savepoints use transient memory, and metadata sharing can cause
conservative conflicts. Performance/refined conflicts remain later work.
