# Step 6: private allocation, inode metadata, and namespace

Implemented on `step6-filesystem-isolation`, 10 November 2026, using the existing
x86 tree. Step 6 extends ordinary syscall routing beyond overwrites: file
extension, create/unlink, directories, hard links, and device-node creation now
use private metadata/data blocks. Console/pipe buffering remains Steps 7-8.

## Implemented behavior

* Members see private directory entries, inode sizes/link counts, allocation
  bitmaps, direct/indirect mappings, and file contents. Outsiders retain the
  public view until durable commit.
* All changed blocks count toward **the same eight-entry cap**. There is no
  separate metadata allowance and no log/cache-cap increase in this stage.
* Commit validates and publishes every private block in one existing exclusive
  log transaction. The Step-5 last-kill check and first-committer-wins validation
  are preserved. A metadata conflict also makes the attempt abort-only.
* Abort or owner death discards private blocks; existing-file handles keep their
  offsets (D27). Abort closes every descriptor alias of a newly allocated file.
  If cwd was newly allocated in the attempt, abort falls back to `/`. Existing
  directory changes to process cwd are local state and are not rewound.
* Successfully committed new-file handles remain usable. An outside descriptor
  already open on a committed-unlinked file retains its contents until close.

## Coarse filesystem gate and cached inode views

`fsview.c` adds a reentrant **sleeping** gate. It is held for each filesystem
operation, not for the lifetime of a speculation. It is also held throughout
publication. The tiny spinlock protecting gate ownership is released before
I/O. The gate is taken before log admission and inode locks; nesting by the same
process is counted. Normal operations are serialized even when the log could
otherwise group them. This is a deliberate concurrency/performance cost, not a
claim of scalable locking.

Namespace syscalls, fstat, chdir and exec enter through `fs_syscall()`. Inode
reads/stats/writes and transaction-based close/exit cleanup also acquire the
gate. Pipes and blocking device reads bypass it so waiting on an internal pipe
does not stop other processes' filesystem progress. Device output still has
ordinary external effects.

`struct inode.view` identifies which view its cached fields describe. `ilock`
reloads from the current private/public inode-block image when invalid or when
the view changes. Shared references remain in xv6's original inode cache: this
preserves existing open-handle lifetime semantics. Cached metadata is only
borrowed for an operation while the gate excludes other filesystem users.
Rollback/completion invalidates the cache; `iput` also reloads the appropriate
view before deciding whether to truncate an unlinked inode. Cancelled private
cleanup never falls back to public truncation.

The gate replaces Step 5's file-only publication lock set: metadata may now
include directories and allocation blocks, so publishing locks only regular
inodes would be insufficient. All filesystem lookups/reads wait behind the
same gate while public cache buffers are installed and made durable.

## Private block buffers and filesystem routines

Filesystem code uses `fs_bread`, `fs_log_write` and `fs_brelse`. For ordinary
operations these delegate to the original buffer cache/log. For speculative
operations, a temporary page contains a private `struct buf`:

1. Read through `shadow_read`; absent means copy the original, while error
   means fail without public fallback.
2. Modify the temporary bytes.
3. `fs_log_write` copies the block into shadow storage, not the log/cache.
4. Free the temporary buffer.

These temporary buffers are not linked into the public buffer cache. `buf.spec`
distinguishes them. Inode, directory, bitmap and indirect-block writes use the
`SH_META` owner tag; ordinary existing-file data keeps its inode owner tag. Raw
test entries with owner zero remain unpublishable.

`balloc` privately changes bitmap bits and zeroes new blocks. `bmap` supports
private direct/indirect allocation. `ialloc`, `iupdate`, `bfree` and `itrunc`
operate on the same view. `readi` includes directories and private indirect
mappings. `writei` supports extension and updates the private inode size only
after the corresponding block chunk succeeds. Namespace syscall helpers now
return errors instead of panicking when private allocation/capacity fails.

Public allocation skips data blocks in the shadow index and inode blocks with
private reservations. An outside unlink also refuses a speculative open inode
or an inode covered by a private inode-block image. This conservatively extends
D20. Unrelated public metadata changes may still proceed, causing original-image
validation to reject a later speculative commit. Sharing an inode/bitmap block
can therefore produce conflicts between otherwise unrelated files.

## Failure handling and operation-local savepoints

Namespace operations take internal savepoints of the current shadow list.
Failure restores the entire operation and invalidates cached metadata. These are
kernel rollback records, **not nested xbegin** or independent user speculations.

Extending writes checkpoint each extending block chunk. If allocation, zeroing,
data writing, or inode-size update fails, that chunk is restored. Earlier
completed chunks remain and the syscall returns their byte count (D22/D25).
Normal short writes remain committable; there is no automatic abort-only flag
for ordinary partial writes.

Final close of a privately unlinked inode is different: the descriptor has
already been removed. If inode/bitmap cleanup cannot fit or allocate, the
cleanup is restored and the context becomes abort-only. This prevents an
orphaned partial allocation view from being published. The owner must abort
or exit. Other namespace failure paths preserve the preceding private view.

Savepoint clones and ephemeral buffer pages increase transient memory use beyond
the eight attached-shadow pages. A namespace operation can fail for memory even
when the attached-shadow count is below eight. The design favors correctness
and simple rollback over memory efficiency.

## Tests and evidence

`spnamespace.c` uses ordinary syscalls and pipe handshakes. It checks public versus
private names/sizes, create/delete commit and abort, direct-to-indirect extension,
directory/cwd behavior, hard links, descriptor aliases, open-unlinked handles,
capacity rollback, partial extension, injected allocation failure, owner death,
and abort-only final-close cleanup.

`tests/run-spec-tests.py --suite namespace-crash` crashes at the four existing
log points and reboots the **same** modified image, validating old/new namespace
and data plus allocator usability. Lifecycle/file tests were updated only where
they formerly required Step 6 mutations to be rejected.

```sh
make -B SHADOW_TEST=1 xv6.img fs.img
python3 tests/run-spec-tests.py --suite namespace --cpus 4 --repeat 3 --log /tmp/ns.txt
python3 tests/run-spec-tests.py --suite namespace-crash --cpus 3 --log /tmp/ns-crash.txt
python3 tests/run-spec-tests.py --suite shadow --cpus 3 --log /tmp/regress.txt
make -B xv6.img fs.img
python3 tests/run-spec-tests.py --suite usertests --cpus 1 --log /tmp/usertests.txt
```

See `step6-verification.md` and `evidence/step6/` for actual results. No commits
are automatically merged into main; review this feature branch first.

## Remaining scope and limits

* Console and external-pipe output remain unbuffered. No exactly-once output
  delivery is claimed; Step 5's durable hook remains the later integration point.
* Reads are not snapshot-isolated for blocks without a private version.
* The coarse gate, full-block metadata validation, and savepoint copying impose
  concurrency, false-conflict and memory costs. Measure/refine them in later work.
* On-disk inode format and log layout are unchanged. Larger attempts still need
  the coordinated capacity/log/cache upgrade. Eight entries can fill quickly.
* Raw test-only shadow operations are not a supported concurrent bypass of the
  filesystem gate while filesystem savepoints are active.
* Disk I/O errors follow xv6's existing panic behavior. Atomic filesystem
  publication does not include console/pipe messages or process-local offsets.
