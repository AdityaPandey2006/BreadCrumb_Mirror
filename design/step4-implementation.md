# Step 4: isolated reads/writes to existing files

Step 4 routes ordinary `read()`/`write()` on **existing regular files**
through the Step-3 shadow storage. A speculating process sees its own
private bytes; every other process keeps reading the public file; `xabort()`
or owner death discards the private bytes and the public file is never
changed. **A commit with private changes still returns -1**: publishing is
Step 5.

Policies used here were agreed before coding and are recorded in the
decision spreadsheet: **D20** (A1, outside-writer refusal), **D21** (B1,
descriptor rule) and **D22** (C1, partial-write rule). Everything else below
is an implementation choice, not an agreed policy.

## Supported subset

| Operation by a speculating process | Step-4 behaviour |
|---|---|
| `read()` of a regular file | Private bytes if this speculation has a copy of the block, otherwise public bytes. A failed/cancelled lookup returns an error. It never falls back to the public view. |
| `write()` inside the current size of a linked regular file | Goes to private copies only. Returns the bytes written. If it fails part-way, it returns the bytes already written, or -1 if none (D22). |
| `write()` that would extend the file (any byte at or past EOF) | -1, nothing written, size unchanged. |
| `open(path, O_CREATE)` | Opens an existing regular file unchanged; -1 if it would create one. |
| `unlink`, `link`, `mkdir`, `mknod` | -1 before any public change. |
| Read/write through a file object opened before `xbegin()` or by another speculation | -1 (D21). `fstat()` still works. |
| Directories opened during the speculation | Readable (public view; directories have no private blocks in Step 4). |
| Console (`T_DEV`) and pipes | **Ordinary effects, not buffered.** Console/pipe buffering are later steps. |
| `exec()` | Reads the binary through `readi`, so it sees the speculation's view. |
| `stat()`/`fstat()` | Public inode metadata; the size never changes in this subset, so it matches the speculative view. |

xv6-public has no `O_TRUNC` or `lseek`; truncation happens only when the
last reference to an unlinked inode is dropped (`iput` → `itrunc`).

Outside processes (ordinary processes, or another speculation once it ever
publishes), per **D20**:

* A write to a block that **any** speculation has privately copied (LOADING
  or READY) is refused. A write that reaches such a block part-way stops
  there and returns the bytes written before it (xv6 short-write
  convention), or -1 if it stopped at the first block.
* An `unlink` of a regular file with **any** privately copied block is
  refused, so a copied block can never be freed and reused by another file.
* Writes to uncopied blocks proceed. Speculative reads are **not a
  snapshot**: a speculation sees outside writes to blocks it has not copied.
* Refusals stop as soon as the speculation's copies are discarded (abort,
  owner death, final member exit).

Two speculations writing the same block each keep their own private copy;
no public state changes, so there is no conflict until a Step-5 commit.

## Filesystem paths traced

`sys_read → fileread → (ilock) readi → bmap → bread → memmove`;
`sys_write → filewrite → begin_op, ilock, writei → bmap (may balloc) → bread →
memmove → log_write; iupdate on extension → iunlock, end_op` (chunked to fit
`MAXOPBLOCKS`). Cached inode metadata (`size`, `nlink`, `addrs`) lives in the
in-memory `struct inode` under its sleeplock; `struct file` holds the shared
`off`. Block allocation is `balloc` (bitmap `log_write`), triggered only by
`bmap` for absent blocks. Durability: `log_write` marks the buffer dirty and
records it in the transaction; `end_op` → `commit` → `bwrite`.

## Design

**Interception point.** Redirection happens in `readi()`/`writei()` (fs.c),
above the buffer cache, only for `T_FILE` inodes and only when
`spec_current()` (proc.c: the caller's `proc.spec`, or 0 before the first
process exists) is non-zero. Directory and inode/bitmap blocks are never
copied in this stage; all metadata stays public and untouched.

**Speculative write path.** It requires `T_FILE`, `nlink >= 1` and
`off + n <= size`. Block lookup uses the new `bmap_existing()`, which never
allocates or writes, then calls `shadow_write()` once per block. It never
calls `begin_op`, `log_write`, `bwrite`, `balloc` or `iupdate`, so no public
buffer is dirtied and the inode is never updated. `filewrite` skips the log
transaction and chunking for speculative callers. Requiring `nlink >= 1`
stops private copies being made of blocks that the last `iput()` of an
unlinked file will free.

**Speculative read path.** `shadow_read()` returns 1 (private bytes), 0 (no
copy: read the public buffer) or -1 (invalid/cancelled → `readi` returns
the bytes already read, or -1). The 0 → public read has no gap, because
every first copy of an inode's blocks happens in `writei` under the same
inode lock the reader holds.

**Original-block path.** `bread_original()` (bio.c) is the explicit
original view. It is identical to `bread()`, because the buffer cache never
does speculative lookup. Its only callers are shadow first-copy
(`shadow.c: access`) and the test-only raw inspections in `shadow_test`.
Step 5 commit/recovery should use it. It is not a path for speculative
writes; its buffers must never be `log_write()`n for a speculation.

**Global shadow index (D20 enforcement).** The outside-writer check must see
copies in every context. Pinning each context to inspect it would raise its
reference count and make a concurrent owner `xcommit`/`xabort` fail their
sole-reference test. Instead, shadow.c keeps every linked entry on a second
list (`gnext`) under a leaf spinlock, `shadowmap.lock`. An entry is added
when its LOADING reservation is linked, *before* the source read, and
removed before it is freed (failed init or discard). Entries now also record
the owning inode number (`inum`, 0 for raw test blocks). `dev`, `blockno`
and `inum` are fixed while an entry is linked.

* `shadow_block_busy(dev, block)` is called by the public `writei` while
  holding that block's buffer lock. A concurrent first copy either is
  already indexed (the outsider is refused) or performs its source read only
  after the outsider releases the buffer, so it copies the new public bytes.
  The private copy can never be based on stale data.
* `shadow_inode_busy(dev, inum)` is called by `sys_unlink` while holding the
  target inode lock, which every first copy of that inode also holds.

**Descriptor rule (D21).** `struct file` gains `spec`, set by `sys_open` to
the opener's speculation ID (0 otherwise; `filealloc` clears it).
`fileread`/`filewrite` refuse a non-`T_DEV` inode when the caller is
speculating and `f->spec` differs. Children forked during the speculation
are members and share its file objects, so private offset movement is
visible only to members. After commit/abort, a former member is ordinary
and can use its files normally. A new speculation cannot use files opened by
an earlier one.

**Partial writes (D22).** Speculative: return the bytes privately written,
or -1 if none; the offset advances by exactly that amount. No up-front check
or rollback: another member can consume free slots concurrently, so a
pre-check could not be reliable. Public `filewrite`: the old
`panic("short filewrite")` is replaced by the same convention, which is only
reachable through a D20 refusal.

**Capacity.** Every modified block counts toward `NSHADOW` (8), including
LOADING reservations. Step 4 introduces no private metadata blocks, so only
data blocks count. Repeated writes reuse an entry.

## Lock ordering (Step 4 additions in bold)

```
inode sleeplock  →  buffer sleeplock (bread)
inode sleeplock  →  ptable.lock (spec_pin/unpin)
inode sleeplock  →  resource_lock  →  **shadowmap.lock** (leaf)
resource_lock    →  ptable.lock      (spec_wait sleep handoff only, Step 3)
**buffer sleeplock → shadowmap.lock** (public-write busy check)
```

Shadow access never runs while holding a buffer lock or a spinlock (the
inode sleeplock is allowed). `shadowmap.lock` is taken alone or as the
innermost lock, and nothing is acquired or slept on while it is held. No
path takes a buffer lock or `resource_lock` while holding `ptable.lock`.
Disk I/O and `kalloc`/`kfree` run outside every spinlock.

## Cancellation and failure

A killed member, or a speculation in ABORTING, gets -1 from `shadow_read` or
`shadow_write` (admission and `killed` are rechecked after the source read,
as in Step 3). `readi`/`writei` then return the bytes already done, or -1.
Nothing ever falls through to a public write. Owner death discards all
entries and removes them from the global index once the last member has
detached. Disk I/O errors still panic, as in xv6.

## Known limitations (documented, not hidden)

1. Console and pipe output from speculating processes is **not buffered**.
2. No create, delete, extension, link, mkdir or mknod inside a speculation.
   They are rejected, so programs that need them fail with -1 (Step 6).
3. Ordinary processes can now get -1 (or a short write) from `write`/`unlink`
   on a file a speculation has copied (the accepted D20 cost).
4. Reads are not snapshot-isolated (see above).
5. If an outsider unlinks a file *before* the speculation copies any of its
   blocks, and a member holds the last reference, then that member's
   `close`/exit frees the inode publicly, as ordinary xv6 does. This
   completes the outsider's unlink; it is not speculative content, and
   private writes to such a file are already refused (`nlink == 0`).
6. Files opened before `xbegin()` must be reopened inside the speculation;
   for example, `cmd > file` redirection gives a refused descriptor.
7. `FSSIZE` was raised from 1000 to 2000 blocks. The new 43 KB `spfiletest`
   binary (xv6 user binaries keep debug info) left only 97 free blocks, and
   the usertests big-file test needs about 142. It panicked with
   `balloc: out of blocks` (raw log in `evidence/step4/development/`). This
   is a disk-size change only. The shadow cap and log size are unchanged
   (R04 still governs their coordinated increase).

## Files changed

`fs.c` (`bmap_existing`, speculative `readi`/`writei`, D20 check, short
public writes), `file.c`/`file.h` (`struct file.spec`, D21, short-write
convention), `sysfile.c` (namespace rejection, speculative `O_CREATE`, D20
unlink refusal, open tagging), `shadow.c` (global index, `inum`, busy checks,
`bread_original` use), `bio.c` (`bread_original`), `proc.c`
(`spec_current`), `main.c` (`shadowinit`), `defs.h`, `param.h` (`FSSIZE`),
`log.c` (test-only public-write counter), `shadowtest.h`/`shadowtest.c`
(new op `ST_PUBWRITES` = 10; comment), `spectest.c` (Step-2 expectation
reversed), new `spfiletest.c`, `Makefile`, `tests/run-spec-tests.py`
(`--suite files`; spfiletest added to the shadow and lifecycle suites).

The syscall ABI is unchanged: 22-26 normal; 27 (`shadowtest`) exists only
in `SHADOW_TEST=1` builds, and it gained op 10.

## Tests

`spfiletest` (normal and test builds) uses only ordinary file calls and pipe
handshakes. It covers:

* outsider sees original bytes vs. private bytes in the speculation
* D20 write refusal, short outsider write and unlink refusal; writes to
  uncopied blocks (non-snapshot)
* parent/child sharing
* two independent speculations
* reads without copies
* cross-block, repeated and indirect-block writes
* the cap: the ninth block fails; a partial write returns 12 and the offset
  moves by exactly 12
* abort preserves the original
* owner exit and owner kill discard the copies, after which outsiders can
  write again
* rejected create/extend/straddle/unlink/link/mkdir/mknod with no public
  trace
* D21 descriptor rule, including member sharing and earlier-speculation
  descriptors
* pipes stay ordinary
* non-speculative behaviour before and after

In `SHADOW_TEST` builds it also injects allocation failure (-1 before any
byte; 12 bytes part-way; no page leak). It also asserts that **no
`log_write()` was made by any speculative process**, which is the single
path for dirtying a public buffer, logging, allocating or updating an inode.

`spectest`'s Step-2 check ("ordinary file write remains visible after
xabort") is reversed. It now prepares the file outside, overwrites it inside
the speculation, aborts, and verifies the original bytes. It also checks
that a new file cannot be created inside a speculation.

Commands and results: see `step4-verification.md`.

## Step-5 integration points

* **Commit publication:** walk the context's entries, write each private
  block through one log transaction (`begin_op`; `bread_original`; copy;
  `log_write`; `end_op`), and wait for the commit to be durable. A block
  stays in the global index until publication completes, so outsiders stay
  refused during commit.
* **Two speculations with copies of the same block**: decide the conflict
  (O02) before publishing one of them.
* Remove the `shadow_count != 0 → -1` gate in `spec_complete`.
* `NSHADOW` must not exceed what one transaction can log (`MAXOPBLOCKS`/
  `LOGSIZE`); raise them together (R04).
* Cancellation during publication (O05) is still open.
