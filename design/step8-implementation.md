# Step 8: Pipe Handling Across Speculation Boundaries

Implemented on branch `step7and8`, 10 November 2026, using the existing x86 xv6-public tree. Step 8 establishes externalization barriers for xv6 pipes across speculation boundaries. Processes inside the same speculation communicate immediately through internal pipes before commit without deadlock. Readers outside that speculation (`spec == 0` or belonging to a different speculation context) are isolated from speculative bytes until durable filesystem commit. Abort, cancellation, and owner termination discard unpublished pipe bytes without leaving blocked processes or dangling pointers.

## Implemented Behavior

* **Dynamic Reader Membership & Visibility Barriers (D40)**:
  - Reader membership dynamically determines visibility across `fork`, `dup`, `close`, `exec`, `xbegin`, and completion.
  - When a speculative process (`spec != 0`) writes to a pipe (`pipewrite`), `pipe_has_external_reader(p, spec)` scans open reader descriptors across all processes in `ptable.proc` under `ptable.lock`.
  - If any open reader descriptor belongs to an external domain (`pr->spec != spec`, covering non-speculative processes and different speculation attempts), the write is intercepted and staged into context-private memory (`sc->pipe_buf`).
  - Internal pipes—where all open readers belong to the exact same speculation—bypass staging and write directly into the pipe's ring buffer (`p->data`), allowing parent/child speculative pipelines to communicate immediately before commit.
  - Ordinary writers (`spec == 0`) write directly to pipes without staging; speculative readers consume ordinary bytes immediately (consumed external input cannot be unread upon abort).
* **Private Pipe Storage & Capacity (D41)**:
  - Private pipe state is allocated lazily as a single 4096-byte page (`kalloc()`) per speculation context (`sc->pipe_buf`, `sc->pipe_len`, `sc->pipe_cap`), protected by `sc->resource_lock`.
  - Storage is entirely separate from the 8-block filesystem shadow pool and the console buffer.
  - Each staged write creates a `struct spec_pipe_record` recording the target `struct pipe*` pointer and payload length, followed immediately by the payload bytes.
* **Overflow, Short-Write Semantics, and Deadlock Prevention (D41)**:
  - Adopted the Unix short-write convention (matching D22/D25/D36): writes buffer as many bytes as can fit in the remaining page capacity and return the count; if remaining capacity is 0 or page allocation fails, the write returns `-1`.
  - Speculative writers to external pipes never sleep waiting for external readers. This completely resolves the fatal progress deadlock hazard with child-exit rules (D06/D08): speculative child writers never block indefinitely on a full pipe waiting for an external reader that cannot read until commit, which would otherwise prevent child exit and make owner commit impossible.
* **Pipe Lifetime & Dangling Pointer Protection (D41)**:
  - Every staged write increments `p->spec_refs` under `p->lock`.
  - When descriptors are closed before commit or abort (`pipeclose`), `struct pipe` is not deallocated while `p->spec_refs > 0`.
  - Deallocation occurs only when both endpoints (`p->readopen == 0 && p->writeopen == 0`) and `p->spec_refs == 0`.
* **Durable Commit Release & Ordered Drain (D42)**:
  - Staged external pipe output is drained in `spec_commit_durable(sc)` strictly after `shadow_publish(sc)` has made filesystem modifications durable on disk (the exclusive transaction log header is written at `write_head()`).
  - The drain iterates through staged records, acquires each target `p->lock`, copies bytes into `p->data`, wakes waiting external readers (`wakeup(&p->nread)`), and decrements `p->spec_refs`.
  - Drainage runs without holding `sc->resource_lock` or the coarse filesystem gate `fs_enter()`. External pipe delivery occurs before console buffer draining.
* **Discard on Abort, Cancellation, and Owner Termination (D43)**:
  - On explicit `xabort()`, owner death or kill, context cancellation (`SPEC_ABORTING`), or failed commit before durability, `spec_pipe_discard(sc)` iterates through staged records, decrements `p->spec_refs`, wakes sleeping readers, frees unreferenced pipe structs, and frees `sc->pipe_buf` via `kfree()`.
  - Discarded bytes never reach external readers. If all writing descriptors are closed, external readers observe EOF (read returns 0) or broken pipe.
* **Ordinary Pipes Unaffected**:
  - Non-speculative pipes retain standard xv6 behavior: 512-byte circular buffer, blocking read/write, and EOF on writer close.

## Crash Semantics and Input Rollback Limits (D44)

* **Crash Window**:
  - Filesystem durability and physical pipe buffer delivery are separate effects.
  - A crash after the log commit header is written preserves filesystem changes across recovery. Because pipe buffers reside only in volatile RAM, staged pipe bytes do not survive reboot. If power fails between disk write and pipe drain, external readers will not receive the pipe bytes upon reboot.
  - No distributed atomic two-phase commit or NVRAM replay queue is claimed.
* **Non-Rollbackable Input Consumption**:
  - Once a speculative process reads and consumes bytes from an external pipe, an abort cannot undo that read or restore the bytes back to the pipe. External input consumption is inherently non-rollbackable.

## Files Modified

* `proc.h`: Added `pipe_buf`, `pipe_len`, `pipe_cap` to `struct spec_context`.
* `defs.h`: Added prototypes for `spec_pipe_drain`, `spec_pipe_discard`, and `pipe_has_external_reader`.
* `pipe.c`:
  - Added `spec_refs` field to `struct pipe`.
  - Implemented `spec_pipe_write()` with lazy page allocation, record framing, and short writes.
  - Implemented `spec_pipe_drain()` to publish staged records into pipe buffers on durable commit.
  - Implemented `spec_pipe_discard()` to discard held records, decrement `spec_refs`, and wake sleepers on abort/kill.
  - Updated `pipealloc()` to initialize `spec_refs = 0`.
  - Updated `pipeclose()` to condition memory deallocation on `spec_refs == 0`.
  - Updated `pipewrite()` to branch to `spec_pipe_write()` when `spec != 0` and `pipe_has_external_reader()` is true.
  - Updated `piperead()` empty condition to wait while `p->writeopen || p->spec_refs > 0`.
* `proc.c`:
  - Implemented `pipe_has_external_reader(p, spec)` scanning `ptable.proc` for open reader descriptors.
  - Hooked `spec_pipe_drain(sc)` into `spec_commit_durable(sc)` after filesystem publication.
  - Hooked `spec_pipe_discard(sc)` into `spec_complete()` and `spec_reclaim()`.
  - Initialized and freed context pipe buffer fields in `spec_context_free_locked()` and `pinit()`.
* `Makefile`: Added `_sppipetest` to `UPROGS`.
* `sppipetest.c`: New unit test suite covering internal pipelines, external commit release, external abort discard, owner kill discard, child-writer/owner-commit ordering, overflow short writes, broken pipes, and ordinary pipe behavior.
* `tests/run-spec-tests.py`: Added `pipe` test suite with host-side regex validation ensuring zero leakage of discarded secrets and presence of committed external messages; updated `shadow` suite.
* `spectest.c`, `spfiletest.c`, `shadowtest.c`, `spnamespace.c`, `spcommit.c`: Updated test synchronization to avoid sending speculative uncommitted handshakes across external pipe boundaries prior to commit or completion.

## Remaining Scope & Step 9 Handoff

* Step 9 will address broader conflict refinement and serialization policies between concurrent speculations and outside filesystem writers.
* Performance measurements and optimization remain for Step 10.

