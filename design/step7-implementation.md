# Step 7: Console Buffering

Implemented on branch `step7`, 10 November 2026, using the existing x86 xv6-public tree. Step 7 ensures that console output produced by speculative processes remains completely private until successful durable filesystem commit. Abort, cancellation, and owner termination discard pending output without external leakage. Non-speculative console output continues to operate immediately.

## Implemented Behavior

* **Speculative Console Isolation**:
  Bytes written to console descriptors (fds 1, 2, duplicated descriptors via `dup`, or directly opened `/console` device nodes) by any process with `spec != 0` are intercepted and held in memory.
  No bytes reach the UART COM1 serial port or CGA display buffer before successful durable commit.
* **Context-Owned Storage & Capacity (D34)**:
  Each active speculation context maintains dedicated console buffer fields (`console_buf`, `console_len`, `console_cap`), protected by the context's `resource_lock`.
  Memory is allocated lazily on the first speculative console write as a single 4096-byte page (`kalloc()`), kept entirely distinct from the 8-block filesystem shadow pool.
  Speculations that do not emit console output incur zero heap allocation overhead.
* **Concurrency & Group Sharing (D05 / D34)**:
  All member processes belonging to the same speculation context (parent and children) write into the shared context-owned buffer.
  Concurrent writes are serialized under `resource_lock`, preserving atomic buffer appending and arrival order across participants.
* **Overflow & Short-Write Semantics (D36)**:
  Adopted the Unix short-write convention (aligning with D22/D25): writes buffer as many bytes as can fit in the remaining 4096-byte page and return the count of buffered bytes.
  If the buffer is full (remaining space is 0) or page allocation fails, the write returns `-1`.
  Under no circumstances do overflow bytes spill out to the physical console.
  Partial writes do not mark the context `abort_only`; the user program inspects the byte count and decides whether to continue or abort.
* **Durable Commit Release (D37)**:
  Buffered console output is drained in `spec_commit_durable(sc)` strictly after `shadow_publish(sc)` has made all filesystem modifications durable on disk (the exclusive transaction log header is written to disk at `write_head()`).
  The drain emits bytes directly via `consputc` under `cons.lock` without holding `resource_lock` or `ptable.lock`, completely avoiding lock inversions and recursive buffering.
* **Discard on Abort, Cancellation, and Owner Termination (D38)**:
  On explicit `xabort()`, owner death or kill, context cancellation (`SPEC_ABORTING`), or failed commit before durability, the held console buffer is discarded and freed with `kfree()`.
  Zero speculative bytes are ever emitted externally.
* **Filesystem Gate Bypass (D35)**:
  In `filewrite()`, speculative device writes bypass the sleeping filesystem gate `fs_enter()`, matching the existing behavior of `fileread()`. Device writes never contend with or serialize on filesystem transactions.
* **Explicit Non-Isolated Effects**:
  Kernel diagnostic messages (`cprintf()`, `panic()`), keyboard driver interrupt echo (`consoleintr()`), and input consumption (`consoleread()`) remain unbuffered.

## Crash Semantics (D39)

Filesystem durability and physical console delivery are separate mechanisms:
* A system crash before the log header reaches disk restores the complete pre-speculation filesystem state on reboot; no console output was released.
* A system crash after the log header is written persists the filesystem mutations across recovery. If power fails before or during console draining, console output may be lost or truncated while disk changes persist.
* Speculative contexts exist only in volatile RAM. No distributed atomic two-phase commit or NVRAM replay queue is implemented, and no persistent exactly-once console delivery is claimed.

## Files Modified

* `proc.h`: Added `console_buf`, `console_len`, `console_cap` to `struct spec_context`.
* `defs.h`: Added prototypes for `spec_console_write`, `spec_console_drain`, and `spec_console_discard`.
* `console.c`:
  - Implemented `spec_console_write(spec, buf, n)` with lazy page allocation and short-write semantics.
  - Implemented `spec_console_drain(sc)` releasing held bytes directly via `consputc()` under `cons.lock`.
  - Implemented `spec_console_discard(sc)` safely freeing the buffer page with `kfree()`.
  - Intercepted speculative writes in `consolewrite()`.
* `file.c`: Bypassed `fs_enter()` in `filewrite()` for `T_DEV` writes when speculative.
* `fs.c`: Updated comment at `writei` for device write routing.
* `proc.c`:
  - Hooked `spec_console_drain(sc)` into `spec_commit_durable(sc)`.
  - Hooked `spec_console_discard(sc)` into `spec_complete()` and `spec_reclaim()`.
  - Initialized and validated console buffer pointers during context allocation and reclamation.
* `Makefile`: Added `_spconsoletest` to `UPROGS`.
* `spconsoletest.c`: New comprehensive user-space test program covering isolation, commit release, abort discard, owner kill discard, parent-child sharing, dup/open descriptors, empty commit, overflow, and durable ordering.
* `tests/run-spec-tests.py`: Added `console` suite with host-side regex assertions ensuring zero leakage of discarded strings and strict arrival ordering of released output; updated `shadow` suite.

## Remaining Scope & Step 8 Handoff

* Step 8 must implement pipe buffering, distinguishing between readers inside the speculation (which communicate immediately without deadlock) and readers outside the speculation (which must be buffered until commit).
* Final outside-writer conflict policies (Step 9) and performance measurements (Step 10) remain to be addressed in future stages.

