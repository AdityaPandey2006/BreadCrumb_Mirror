# Step 9: Complete Conflict Handling and Concurrency Validation

Implemented on branch `step9`, 10 November 2026, using the existing x86 xv6-public tree. Step 9 establishes the comprehensive conflict detection and resolution policy, validates concurrency across multiple participants and independent speculations, documents the formal consistency contract, and ensures that concurrent activity produces either a valid, complete, durable commit or the documented abort outcome—never silent lost updates, partial publication, premature output, deadlock, or resource use-after-free bugs.

---

## 1. Supported Consistency and Isolation Contract

### What Is Isolated
* **Private Modifications:** All speculative writes to file data, directory entries, disk inodes (`struct dinode`), indirect blocks, and allocation bitmaps are directed exclusively to dynamically allocated shadow blocks (`shadow_write`). Up to `NSHADOW = 8` blocks per speculation context are supported.
* **Namespace and Metadata:** Created names, unlinked files, extended file sizes, and directory contents are visible only to processes participating in the same speculation.
* **Cached Inode Views:** In-memory inodes (`struct inode`) tag their cached fields with a `view` ID corresponding to the active speculation. `ilock()` reloads fields from the current private shadow or public block whenever the view changes or is invalidated.
* **External Outputs:** Console output (`write` to stdout/stderr, `/console`, or dup'd console descriptors) and external pipe writes (where any open reader belongs outside the speculation) are staged in context-private memory and withheld until durable commit.

### When Reads Observe Outside Changes (Non-Snapshot Reads)
* **Direct Public Fallback:** Speculative reads from a block that has a private shadow return the private shadow's data (`shadow_read`). Speculative reads from any block that has *not* been privately shadowed fall back directly to the public buffer cache (`bread_original`).
* **No Snapshot Isolation:** Speculative reads do **not** record a read-set, do **not** acquire read locks, and do not snapshot disk blocks. If an outside process or a committed peer modifies an un-shadowed block, a speculative process will observe those outside modifications immediately upon subsequent reads.
* **Explicit Non-Claims:** The xv6 speculation barrier does **not** provide Snapshot Isolation (SI), read-set validation, or Serializability (SR). It provides **write isolation with first-committer-wins validation**.

### What Counts as a Conflict
* **Commit-Time Data Conflict:** At commit time (`shadow_publish`), the kernel compares every shadowed block byte-for-byte against the saved `s->orig` image captured when that shadow block was first copied (`memcmp(b->data, s->orig, BSIZE) == 0`). If any byte in the public block differs from `orig`, validation fails with `SP_CONFLICT`.
* **Commit-Time Metadata False Conflict:** Because xv6 disk inodes are 64 bytes (`IPB = 8` dinodes per 512-byte block) and directory entries share directory blocks, two independent speculations modifying different files within the same disk block conflict at the block boundary. The second committer observes a byte mismatch on the shared block and is rejected.
* **Outside Operation Collision:** An outside write attempting to touch any block currently shadowed by an active speculation, or an outside unlink attempting to delete an inode that has shadowed blocks or active speculative open descriptors, is refused.

### Refused Outside Operations
* **Outside Writes (D20):** In `writei()`, an outside process attempting to write to a block marked busy in `shadowmap` (in `SH_LOADING` or `SH_READY` state) is refused, returning a short write or `-1`. The public block remains untouched, and the active speculation continues normally.
* **Outside Unlink / Freeing (D20/D33):** In `sys_unlink()`, an outside process attempting to unlink a file with shadowed blocks (`shadow_inode_busy`) or open speculative file descriptors (`file_spec_busy`) is refused with `-1`.
* **Public Allocation (D33):** `ialloc()` skips dinode blocks that are busy in `shadowmap`; `balloc()` skips data blocks that are busy in `shadowmap`.

---

## 2. Conflicting Access Path Audit

1. **Multiple Members Modifying the Same Private Block:**
   - Members of the same speculation serialize file mutations under the target `ip->lock` and `fs_enter()`.
   - Entry initialization in `shadow.c:access()` uses `SH_LOADING` reservations under `sc->resource_lock`. Concurrent members waiting for a loading block sleep via `spec_wait()`, preventing duplicate initialization or torn updates.
   - Once `SH_READY`, modifications update `s->data` under `sc->resource_lock`.
2. **Independent Speculations Modifying the Same Block:**
   - Both speculations privately allocate shadows and record identical `orig` images from the public disk.
   - At commit time, the first speculation takes the exclusive log transaction (`begin_op_exclusive()`), validates `memcmp`, and writes to the log.
   - The second speculation subsequently validates its `orig` against the newly published public block. The `memcmp` fails, returning `SP_CONFLICT`.
   - The loser sets `sc->abort_only = 1`, and `xcommit()` returns `-1`. No partial data is written, and the loser can only `xabort()` or exit.
3. **Different Files Sharing Inode, Directory, or Bitmap Blocks (False Conflicts):**
   - When Speculation A creates `/fileA` and Speculation B creates `/fileB`, both update the root directory block and possibly the same dinode block `IBLOCK()`.
   - Under full-block original-image validation, the first to commit publishes the shared metadata block. The second committer's `memcmp` detects the modified dinode or directory entry and safely aborts.
   - This prevents lost directory entries or corrupted inode tables without complex sub-block lock management.
4. **Outside Writes, Allocation, and Unlink:**
   - Public writers check `shadow_block_busy()` while holding the target buffer lock. A concurrent first-copy either finishes and indexes the block (causing the outside write to be refused) or the outside write completes before the first-copy reads `orig`.
   - Public `ialloc` and `balloc` skip busy blocks, preventing public reallocation of blocks held in private shadow views.
5. **Open Descriptors Across Commit, Abort, and Unlink:**
   - File descriptors are tagged with their opening speculation ID (`f->spec`). Speculative processes cannot read/write non-device files opened before `xbegin` (D21).
   - On abort, `fs_finish()` automatically closes all open file descriptor aliases of newly allocated files (`shadow_inode_new`) and falls back cwd to root if newly allocated.
   - On commit, tags are cleared (`f->spec = 0`), and handles remain valid.
   - If a committed speculation unlinks a file while an outside process holds an open descriptor, xv6 inode reference counting (`ip->ref`) ensures the file contents remain accessible to the outsider until final `close()`.
6. **Console and Pipe Buffers on Conflict:**
   - Staged console output and external pipe output are held in `sc->console_buf` and `sc->pipe_buf`.
   - `spec_commit_durable()` is invoked **strictly after** `shadow_publish()` succeeds and the exclusive log transaction is durable at `write_head()`.
   - If validation fails (`SP_CONFLICT`), `spec_commit_durable()` is skipped. When the owner calls `xabort()` or exits, `spec_console_discard()` and `spec_pipe_discard()` free all staged buffers with `kfree()`. Zero bytes leak to serial, screen, or external pipe readers.

---

## 3. Synchronization and Lock Hierarchy

To prevent deadlocks and race conditions, locks must strictly be acquired in the following order:

```
1. gate.lock (spinlock)
   └─> 2. Filesystem Gate (fs_enter / fs_leave, sleeping lock)
         └─> 3. Inode sleep-lock (ip->lock)
               └─> 4. Exclusive log transaction (begin_op_exclusive / log.lock)
                     └─> 5. Buffer sleep-lock (bp->lock)
                           └─> 6. Process table (ptable.lock, spinlock)
                                 └─> 7. Context resources (sc->resource_lock, spinlock)
                                       ├─> 8. Shadow map index (shadowmap.lock, leaf spinlock)
                                       ├─> 9. Pipe lock (p->lock, spinlock)
                                       └─> 10. Console hardware (cons.lock, spinlock)
```

### Critical Synchronization Rules
* **No Sleeping Under Spinlocks:** Spinlocks (`gate.lock`, `ptable.lock`, `sc->resource_lock`, `shadowmap.lock`, `p->lock`, `cons.lock`) are never held across disk I/O, `kalloc()`, or sleeping operations.
* **Sleep Handoff Protocol (`spec_wait`):** To avoid missed wakeups when waiting on `SH_LOADING` entries, `spec_wait()` acquires `ptable.lock` and releases `sc->resource_lock` before sleeping on `&ptable.lock`. This is the sole documented nesting between resource and process table locks.
* **Exit Path Protection:** In `proc.c:exit()` and `free_embryo()`, closing open file descriptors (`fileclose`) and releasing `cwd` (`iput`) are protected under `fs_enter() ... fs_leave()`. This eliminates race conditions between asynchronous process termination, cached inode view invalidation, and active filesystem mutations.
* **Ordered Durable Drain:** In `proc.c:spec_commit_durable()`, staged external pipe records are drained (`spec_pipe_drain`) strictly before console output (`spec_console_drain`), fulfilling Decision D42.

---

## 4. Deterministic Concurrency Test Suite (`spconflict.c`)

The new test suite `spconflict.c` exercises 9 deterministic concurrency scenarios:
1. `test_data_conflict()`: Two independent speculations modify block 0 of `cf_data`. Spec A commits first (returns 0); Spec B attempts commit, fails with `-1`, verifies `abort_only` on second commit attempt (`-1`), and successfully aborts (`xabort() == 0`). Public file holds A's data only.
2. `test_metadata_false_conflict()`: Spec A creates `cf_meta_a` and Spec B creates `cf_meta_b` in the root directory. Spec A commits first; Spec B suffers a metadata false conflict on the directory block, fails commit, and aborts. `cf_meta_a` is published; `cf_meta_b` is cleanly discarded.
3. `test_outside_writer_refusal()`: Active speculation modifies block 0 of `cf_race`. Outside process attempts write to block 0 (refused, returns `<= 0`) and attempts unlink (refused, returns `-1`). After speculative commit succeeds, outside write and unlink succeed normally.
4. `test_shared_participants()`: Owner begins shared speculation, creates file, forks Child 1 and Child 2. Both children write private data and exit. Owner commits; all participants' changes are published durably.
5. `test_live_peer_rejection()`: Owner attempts `xcommit()` and `xabort()` while child participant is alive; both return `-1` without disturbing speculation state. After child exits and is reaped, owner commits successfully.
6. `test_owner_kill_cancellation()`: Speculative child creates doomed file and sleeps; parent kills child. Context is cancelled (`SPEC_ABORTING`), private changes are discarded, and a new speculation starts cleanly.
7. `test_context_reuse()`: 30 consecutive commit/abort cycles verify that context slots (`NSPEC = 8`), shadow entries, and memory pages are reclaimed without leaks.
8. `test_conflict_pending_output()`: Spec A writes secret tokens to an external pipe and console, and modifies a file. Spec B modifies the same file and commits first. Spec A attempts commit, suffers conflict, and aborts. External pipe reader observes 0 bytes (EOF); serial log assertions verify zero console leakage.
9. `test_multicpu_concurrency()`: 4 concurrent processes compete on speculative filesystem operations under multi-CPU SMP, verifying progress and absence of deadlocks.

---

## 5. Decision Records Added

* **D45 (O02 Finalization):** First-committer-wins at whole-speculation granularity based on 512-byte original-image validation; losing committer becomes abort-only. Outside writes/unlinks to shadowed blocks/inodes refused with `-1` (D20/D33).
* **D46 (Consistency Contract):** Write isolation with first-committer-wins validation; non-snapshot reads. Metadata false conflicts occur at block boundaries.
* **D47 (Gate Coverage):** Coarse filesystem gate retained as correctness baseline; `exit()` and `free_embryo()` file/cwd cleanup wrapped in `fs_enter() ... fs_leave()`.
* **D48 (Drain Order):** Staged external pipe output drained strictly before console buffer drain in `spec_commit_durable()`.

---

## 6. Remaining Scope and Step-10 Handoff

* **Global Gate Performance Cost:** The coarse sleeping filesystem gate serializes all filesystem operations. While optimal for correctness, it limits throughput under heavy concurrent filesystem workloads.
* **Metadata False Conflicts:** Sharing of 512-byte dinode blocks or directory blocks causes independent speculations to conflict even when operating on different filenames.
* **Fixed 8-Block Shadow Ceiling:** Commit transaction footprint remains capped at 8 blocks.
* **Step-10 Handoff:** Step 10 will perform systematic performance benchmarking (1 KiB–64 KiB file operations, concurrency scaling, capacity cliffs) and final failure analysis.

