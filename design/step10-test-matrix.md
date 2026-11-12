# Step 10 failure-test matrix

Each row is one failure case named in the Step-10 brief, the test(s) that
exercise it, the outcome that is checked, and the harness suite that runs it.
"FS" is the filesystem outcome; "R/P" is the resource or progress outcome.
Suites are run by `tests/run-matrix.sh`; per-run results are in
`design/evidence/step10/matrix/matrix.tsv` (see `step10-verification.md`).
New Step-10 tests are marked **(new)**. Tests marked *test build* need
`SHADOW_TEST=1` (fault injection / crash points); all others run in both builds.

| # | Case | Test(s) | Checked outcome (FS / R/P) | Suite(s) |
|---|---|---|---|---|
| 1 | Nested `xbegin` (owner and inherited child) | `spectest` "nested begin is rejected", "inherited child rejects nested begin"; `spfailure` F7 **(new)** | FS: unchanged; R/P: -1, existing speculation and its changes kept | lifecycle, shadow, failure |
| 2 | Unauthorized commit/abort (non-owner member) | `spectest`, `spcommit` "non-owner member cannot commit or abort"; F7 **(new)** | -1, attempt unchanged | lifecycle, commit, failure |
| 3 | Completion with live peers | `spectest`, `spconflict` T5, F7 **(new)** | -1 for both commit and abort; succeeds after peer exits | lifecycle, conflict, failure |
| 4 | Child (member) exit | `spcommit` "parent sees exited child's private bytes", `shadowtest` "member exit retains shared shadow contents" | member changes retained and committed by owner | commit, shadow |
| 5 | Owner voluntary exit | `spectest` owner cancellation(0); `spfiletest` "owner exit discards"; F3 **(new)** | FS: private file discarded; R/P: held console + pipe bytes never delivered (LEAK_EXIT_* absent), outside reader gets EOF | lifecycle, files, failure |
| 6 | Owner kill | `spectest` owner cancellation(1); `spfiletest`, `spconsoletest`, `sppipetest`, `spconflict` T6 | changes discarded, held output discarded, context reusable | lifecycle, files, console, pipe, conflict |
| 7 | Cancelled sleepers | `spectest` "owner exit/kill wakes and terminates sleeping member"; `shadowtest` "killed LOADING waiter exits"; F2 **(new)** | R/P: members in `sleep(1000)` and blocked on an internal-pipe read are woken and exit within 300 ticks of the owner kill (measured: see verification) | lifecycle, shadow, failure |
| 8 | Shadow capacity exhaustion | `spfiletest` ninth block refused / partial (12 bytes); `spcommit` cap + short write; `spnamespace` capacity rollback; demo 6a; fig 1-2 | FS: never spills to the public disk; R/P: short write count or -1 | files, commit, namespace, demo, bench |
| 9 | Console capacity exhaustion | `spconsoletest` overflow (96-byte short write, then -1); bench console 6144 B | held page capped at 4096 B; nothing reaches UART | console, bench |
| 10 | Pipe capacity exhaustion | `sppipetest` overflow (88-byte short write); F6 **(new)** full outside pipe: held write returns without sleeping; F1 **(new)** commit releases 1500 B into a 512 B pipe | no sleep while held; release waits for the reader without blocking the filesystem | pipe, failure, drainfull |
| 11 | Allocation failures | `shadowtest`, `spfiletest` shadow page; `spnamespace` injected failure; F8 **(new)** held-console and held-pipe page (`ST_FAIL` selector) | -1 for the failing write, next write succeeds, no page leaked (exact free-page count) | shadow, files, namespace, failure (test build) |
| 12 | Failed namespace operations | `spnamespace` capacity rollback, full-table unlink rollback, injected allocation failure | operation restored to savepoint; name/inode/bitmap consistent | namespace |
| 13 | Partial writes | `spfiletest` partial; `spcommit` "commit after a refused block and a short write (D25)"; `spnamespace` partial extension | exact byte count returned; prefix committable | files, commit, namespace |
| 14 | Abort-only cases | `spcommit`/`spconflict` loser of first-committer-wins; `spnamespace` failed final-close cleanup | commit -1 repeatedly, abort succeeds, nothing published | commit, conflict, namespace |
| 15 | Conflicts | `spconflict` T1-T3, T8; `spcommit`; demo 6b/6c; fig 7 | first committer wins; outside write/unlink to copied block refused; loser output discarded | conflict, commit, demo, bench |
| 16 | Repeated context reuse | `spectest` slot reuse + context table full; `shadowtest` 100 cycles; `spconflict` 30 cycles; F9 **(new)** 20 mixed commit/abort cycles with files, dirs, held console and pipe | R/P: free-page count before == after (exact), no shadow pages | lifecycle, shadow, conflict, failure |
| 17 | Open descriptors | `spfiletest` D21 descriptor rule; `spnamespace` new-descriptor abort/commit | pre-xbegin descriptor refused; aborted new-file aliases closed | files, namespace |
| 18 | cwd | `spnamespace` aborted new cwd falls back to `/` | cwd valid after abort | namespace |
| 19 | Unlink lifetime | `spnamespace` "outside open descriptor retains unlinked contents" | committed unlink keeps open public handle readable | namespace |
| 20 | Console/pipe cleanup | `spconsoletest`, `sppipetest` abort/kill discard; F3, F8 **(new)** | held pages freed, LEAK_* tokens absent from serial log | console, pipe, failure |
| 21 | Blocked reader | F4 **(new)** outside reader asleep before bytes are held; commit delivers exactly the message, abort gives EOF | no early wake-up with data; correct delivery | failure |
| 22 | Blocked writer | F5 **(new)** outside writer blocked on a full pipe whose reader is a speculative member | writer completes (no deadlock); consumed input not restored by abort (D44) | failure |
| 23 | Crash before / after the durable point | `spcommit` crash points 1-4; `spnamespace` crash points 1-4 | same `fs.img` rebooted: old state at 1-2, new state at 3-4, allocator usable | crash, namespace-crash (test build) |
| 24 | Crash during output release | `spfailure outcrash` points 5-8 **(new)** | FS: complete new state at every point; console: none (5-7) or exactly a 16-byte prefix (8); pipe delivery recorded; lost output never replayed after reboot | output-crash (test build) |
| 25 | Filesystem layout with a full root block | `tests/regress-root-hole.sh` **(new)** | stock mkfs rounding reproduces the abort data loss; fixed mkfs passes | root-hole regression |
| 26 | Standard xv6 behaviour | `usertests` (test, normal and original-xv6 builds) | ALL TESTS PASSED | usertests |
| 27 | Host agent | `python3 -m unittest discover -s agent` (18 tests, 3 new for the speculation template) | OK | agent-unittest |

## Known test limitations

* Several older tests order an outside process after a speculation with a
  fixed `sleep()` because a speculative process cannot signal an outsider
  without externalizing. One of them (`spfiletest` outsider isolation) failed
  intermittently in Step 10 and was replaced by a deterministic probe
  (defect D3). The other sleep-based orderings did not fail in any Step-10 run
  but remain timing assumptions.
* Pipe delivery at crash points 6-7 is observed, not asserted: whether the
  outside reader runs before the halted machine stops is scheduling-dependent.
* Crashes are injected by `panic` in the test build at named points; real
  power loss inside a single disk write (torn sector) is not modelled (QEMU
  raw image, synchronous IDE writes).
