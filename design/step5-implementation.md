# Step 5: abort, then durable commit

`xabort()` discards everything a speculation did. `xcommit()` publishes the
speculation's private file blocks to the real filesystem in **one exclusive
log transaction**. It returns 0 only after that transaction's commit record
is on disk. Outside readers see either the complete old contents or the
complete new contents, never a mix and never bytes that are not yet
durable. After a crash, log recovery gives the complete old state or the
complete new state.

Scope is unchanged from Step 4: overwrites of existing regular files.
Extension, create and delete are still rejected inside a speculation
(Step 6). Console and pipe output are still unbuffered (Steps 7-8).

## Design decisions (agreed 3 Nov 2026; spreadsheet rows D23-D27)

Each entry gives the rule, the reason, where it lives in the code, and what
changing it would involve.

### D23 — owner killed during commit: kill wins until publication starts
* **Rule.** Commit checks `myproc()->killed` one last time, after taking the
  inode locks and passing validation and just before the first public buffer
  is changed. That check is the **point of no return**.
  * Killed before it: no public change; the context becomes ABORTING and the
    owner's exit discards the private blocks.
  * Killed after it: the commit completes durably and the process then dies
    on its way back to user mode. It never sees the 0, but the commit
    happened and is never reported as aborted.
* **Why.** In xv6 a `log_write()`n buffer is part of the shared transaction
  and cannot be taken back, so "kill wins until durable" is not possible
  without rewriting the log.
* **Code.** `shadow.c: shadow_publish()`, the line
  `if(r == SP_OK && myproc()->killed) r = SP_KILLED;`. `proc.c:
  spec_complete()` maps `SP_KILLED` to `spec_cancel_locked()`. `kill()`
  still never cancels a COMMITTING context; it only sets `killed`.
* **To change.** For "commit wins once reserved" (A2), delete that check. To
  move the point of no return later, logged buffers would first need to be
  removable from the log, which is a large change.

### D24 — commit-time conflict between speculations: first committer wins
* **Rule.** Each private copy stores the public bytes it was copied from
  (`struct shadow.orig`, saved at first copy). At commit, every public block
  is compared with its `orig`. Any difference means another speculation
  committed first: `xcommit` returns -1, nothing is published, and the
  context is marked `abort_only`. After that, `xcommit` keeps returning -1;
  only `xabort` (or exit) ends it. Reads still work.
* **Why.** Optimistic concurrency. Step 4 behaviour (two speculations may
  both privately write a block) is unchanged, and no outside writer can
  cause it because D20 refuses them.
* **Code.** `shadow.c: access()` (saves `orig`); `shadow_publish()`
  (validation loop, `SP_CONFLICT`); `proc.h` (`abort_only`);
  `proc.c: spec_complete()` (sets and checks `abort_only`).
* **No validation-to-publication race.** Validation and installation run
  inside the same exclusive transaction with the inode locks held. D20 stops
  outsiders writing copied blocks, and only one publication can own the log
  at a time.
* **To change.** For block ownership at copy time instead (B2), refuse
  `shadow_write` when another context's index entry has the same block, then
  drop the validation. Also update spfiletest's "independent speculations"
  test.

### D25 — short writes do not make a speculation abort-only (resolves R03)
* **Rule.** No flag is set. A program that ignores a short write count (D22)
  can commit a half-updated private view.
* **Code.** None; it is the absence of a flag. **To change:** set
  `abort_only` (under `ptable.lock`) when `writei`'s speculative loop returns
  short.

### D26 — crash meaning (part of O05)
* **Rule.** Crash before the commit record (`write_head` of the
  speculation's own transaction) reaches disk: recovery gives the complete
  old state, and the speculation is gone (contexts are memory-only). Crash
  after it: recovery gives the complete new state, even if `xcommit` never
  returned to the caller. No exactly-once notification is claimed.
* **Code.** `log.c: begin_op_exclusive()` makes the publication the only
  transaction in its log group, so the publisher's own `end_op()` runs
  `commit()`. Crash test points are in `commit()` and `install_trans()`
  (test build only).

### D27 — file offsets are not rewound on abort
* Offsets belong to the members' file objects, not to the public file. No
  code. To change, save `f->off` for D21-tagged files at first use and
  restore it on abort.

## Commit state machine

```
            xcommit() by owner
ACTIVE ─────────────────────────► (checks fail) return -1, nothing changed
   │  ptable.lock: owner, sole member, refs==1, !abort_only
   ▼
COMMITTING  (+1 exclusive reference; new pins refused => nothing in flight)
   │  shadow_publish(), no spinlock held:
   │    begin_op_exclusive()                       wait for the log to drain
   │    iget+ilock affected inodes (inum order)
   │    validate each block == orig ──────────────► SP_CONFLICT
   │    killed? ──────────────────────────────────► SP_KILLED
   │  ===== point of no return =====
   │    install: bread_original, copy, log_write   (into public cache)
   │    end_op() -> commit(): write_log, write_head  <= DURABLE
   │                          install_trans, clear log
   │    iunlock; iput (ordinary transaction)
   ▼
spec_commit_durable()          hook for later console/pipe release
shadow_discard(): unindex + free private copies (outsiders allowed again)
detach owner (non-speculative), unpin, return 0

SP_CONFLICT  -> ACTIVE + abort_only, return -1
SP_KILLED    -> ACTIVE then ABORTING (spec_cancel_locked), return -1, exits
SP_UNSUPPORTED (raw test blocks with no owning file) -> ACTIVE, return -1
```

The context cannot be freed during publication: the completion reference
keeps `refs > members`, and the owner is still a member. Raw shadow-test
blocks (inum 0) are never published. Step 3's "commit rejected without
losing work" check still holds for them.

## Abort and cleanup

Private state in the supported subset is only the shadow entries; file
sizes and inodes are never changed privately. `shadow_discard()` removes
each entry from the global index and frees its page. It runs for explicit
`xabort`, after a successful commit, and in final reclamation after owner
exit/kill or the last member's exit. Membership is detached only after
discard, so cleanup never becomes a public write. Waiters on a LOADING
entry are woken by Step-3 cancellation handling. Allocation failures leave
no entry (Step 3/4).

## Log admission and durability (`log.c`)

A normal `end_op()` commits only when the last outstanding operation ends.
Returning from it does **not** prove that the caller's blocks are on disk.
Publication therefore uses `begin_op_exclusive()`:

* `log.exclusive` has three states: `EXCL_NONE`, `EXCL_WAITING` (admission
  closed; waiting for the current group to drain) and `EXCL_OWNED` (the
  publication is the only transaction).
* `begin_op()` waits while `exclusive != NONE`. Only the OWNED
  transaction's own commit returns the state to NONE.
* Invariant (all builds): `log_write` panics if `exclusive == OWNED` and
  `outstanding != 1`.

**Development bug found by the tests.** The first version cleared the flag
after *any* commit, including a group commit that finished while the
publisher was still waiting. Ordinary operations could then join the
"exclusive" transaction. The test-build counter of stray speculative
`log_write()` calls caught it intermittently. Fixed with the three states;
the regression test is `spcommit`'s concurrent ordinary-writer case. See
step5-verification.md.

## Live-reader isolation

The affected inodes stay locked from before the first public change until
after the commit record is durable. `readi` (including `exec`) holds the
inode lock, so readers see all-old or all-new contents and never see bytes
that are not yet durable. Holding inode locks across `end_op()` is safe
only because the transaction is exclusive: no other operation in it can be
waiting for those locks. Ordinary writers and opens block in `begin_op()`
for the short publication window.

## Capacity audit (this tree)

| Item | Value | Step-5 use |
|---|---|---|
| `MAXOPBLOCKS` (per-op reservation) | 10 | publication writes ≤ `NSHADOW` = 8 data blocks + 0 metadata |
| `LOGSIZE` / on-disk log / header | 30 blocks; header holds 30 numbers (< 512 B) | ≤ 8 entries, log empty at exclusive start |
| `NBUF` | 30 | ≤ 1 buffer held at a time during validate/install; `commit()` needs 2 |
| Inode cache | `NINODE` 50 | ≤ 8 extra references |

Step 4 never changes size, inode, bitmap or indirect blocks, so the
footprint is data blocks only, and it is never split across transactions.
Raising `NSHADOW` above `MAXOPBLOCKS` would need a matching reservation
change (R04). Stock xv6 already has `NBUF == LOGSIZE`, a theoretical tight
spot when the log is completely full; this is unchanged and not hit by the
tests.

## Lock order (Step 5 additions in bold)

```
log admission (begin_op / **begin_op_exclusive**)  ->  inode sleeplocks
**publication: inode sleeplocks in inode-number order (all T_FILE)**
inode sleeplock -> buffer sleeplock -> shadowmap.lock (leaf)
inode sleeplock -> resource_lock -> shadowmap.lock
resource_lock -> ptable.lock only in spec_wait (Step 3)
**inode sleeplocks held across the exclusive end_op()/commit()**
```

Directories are never locked by publication. Other code that holds two
inode locks always takes a directory first, so locking files in inode-number
order cannot deadlock with it. No spinlock is held across disk I/O.

## Test instrumentation (SHADOW_TEST=1 only; syscall 27)

| Op | Meaning |
|---|---|
| `ST_PUBHOLD` (11) value 1 or 2 | pause publication before the point of no return (1) or after installing its first block (2); 0 releases; a kill also releases |
| `ST_PUBHELD` (12) | which hold point a publisher is paused at |
| `ST_CRASH` (13) value N | halt the machine at crash point N of the next publication: 1 before log blocks, 2 before the commit record, 3 after it, 4 after installing the first of several blocks |

The harness's `--suite crash` sets up files, crashes, then **reboots the
same `fs.img` copy** and checks the result.

## Files changed in Step 5

`log.c` (exclusive transaction, invariant, crash points),
`shadow.c` (`orig`, `shadow_publish`, hold/crash test support),
`proc.c`/`proc.h` (state machine, `abort_only`, `spec_commit_durable`),
`fs.c` (`iget_ref`), `defs.h`, `shadowtest.h` (ops 11-13), `shadowtest.c`
(label), `spfiletest.c` (removed the Step-4 "commit unsupported" check), new
`spcommit.c`, `Makefile`, `tests/run-spec-tests.py` (refactored into a
`Guest` class; `commit` and `crash` suites).

## Remaining limitations

* No extension, create, delete, link or mkdir inside a speculation (Step 6).
* Console and pipe output are not buffered; `spec_commit_durable()` is the
  release point to use later. No exactly-once output delivery is claimed.
* While a publication owns the log (a few block writes), ordinary file
  operations and console writes wait.
* Reads are not a snapshot (Step 4).
* O02 (broader conflict policy) and O05 (output delivery at a crash) remain
  partly open.

## Next: Step 6

Isolate file extension, creation and deletion: private inode/size/bitmap/
directory changes, which count toward the 8-block cap, and their
publication through the same exclusive transaction.
