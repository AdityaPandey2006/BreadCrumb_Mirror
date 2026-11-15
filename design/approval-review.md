# Approval-channel fixes (D64-D66)

This note supersedes the optional, owner-written size-only review described in
the historical D63 summary. The spreadsheet records the problems and decisions.

## One question and one answer at a time

A reservation protected by `cons.lock` covers the whole approval exchange.
A second owner waits before printing. Ordinary console readers also wait, so
neither another speculation nor the shell can consume the current answer.
Sleeping releases the spinlock, and no filesystem gate is held during review.
Killing the active or queued owner releases its reservation/waiter count and
wakes other callers. Complete and partially typed old input is discarded.
Each prompt carries its speculation ID in a kernel-generated frame. The host
recognizes that frame, rather than matching arbitrary `commit?` text.

## File summary and current-public block comparisons (current version)

At xbegin the kernel captures names, parent inode identities, inode numbers,
file types and sizes, plus a compact allocated-inode map. No file contents or
original block images are retained. At review it captures the private namespace
and matches full paths against the initial metadata: missing -> present is
Created, present -> missing is Deleted, and changed inode/type/size is Modified.
For an unchanged inode and size it compares current public file bytes with the
private file bytes, including direct and indirect blocks. Same-size ABC -> CAT
is therefore Modified. Unlink/recreate at the same path is Modified, hard-link
aliases are listed, and create-then-delete has no final file entry. Paths are
reconstructed in the kernel, not supplied as an authoritative command list.
The summary covers entries relevant to shadowed directory/inode/data blocks.

The lower-level inventory compares each private block with the current public
buffer-cache block (bread_original), NOT a saved original and NOT an uncached
raw disk read. The filesystem gate stabilizes both views while the report is
built. Before/after previews are escaped and limited to 24 bytes starting at
the first changed byte. All changed blocks, counts, inode metadata changes,
and held console/pipe counts remain listed. Buffered payloads stay hidden.
The xask note is a label, not the authoritative report.

This intentionally combines initial namespace metadata with current public
content, rather than a snapshot of the filesystem at xbegin. A rival commit
can change the public content shown as "before". Review does not authorize
silently overwriting that rival: each shadow records a 32-bit public generation
at first copy, and every real log_write increments that block's generation.
Publication rejects changed generations, including an ABA change back to the
same bytes, and counter exhaustion fails closed until reboot. Recovery starts
with no live speculations, so generations need not persist across reboot.
The generation array is 4 * FSSIZE bytes (8000 bytes at the current cap), shared
by all contexts; each shadow retains one 4-byte generation instead of 512
original-content bytes. Conservative conflicts are possible for no-op public
writes. Existing outside-writer restrictions are retained.

Limits: one 4 KiB initial metadata page per context, at most 112 non-dot
namespace entries and 256 inode identities, with paths bounded to 127 bytes.
An over-capacity metadata snapshot refuses xbegin. Temporary private metadata
and text pages are allocated for review; overlong paths, allocation failure,
or a summary/block inventory that does not fit refuses approval. No truncated
report is accepted. Shadow entries are still allocated one page each, so
removing original bytes does not yet reduce their physical page count. This
new metadata page can increase physical memory use despite smaller entries.

## Every commit requires approval

`xcommit` requires both asked and approved. An eligible direct call automatically
opens the same review when needed; the kernel never supplies yes. A no answer
leaves the attempt frozen and uncommitted until abort/exit. `specrun commit`
and successful `keep` candidates explicitly request review too. Abort needs no
approval. Frozen owners cannot modify the work they showed the user.

Normal rule-based agent invocations ask a human. `--answer y` or `--answer n`
is an explicit scripted-answer option for controlled demos. The kernel has no
test bypass. Test drivers record their scripted yes answers in raw logs.

Benchmarks that commit now include the review/report and host-answer overhead.
Older committed performance data remains historical evidence for its recorded
build, not a measurement of this new policy. Timing tests cannot attribute
human decision time solely to filesystem commit cost.

## Checks

`tests/run-spec-tests.py --suite approval` covers competing owners, a killed
active owner with another waiting, an ordinary competing console reader,
same-size content reporting, direct commit yes/no and report overflow. The
existing `ask` suite checks owner/participant restrictions, frozen syscalls,
abort, owner kill and conflict validation after approval. Both are included in
the normal and instrumented correctness matrix.

## Historical verification (before the current-public comparison change)

The expanded default matrix passed **45/45 rows**, including the new ask and
approval suites in normal and instrumented builds, combined regressions on
1-4 CPUs, filesystem/output crash recovery and four xv6 usertests runs.
All 20 agent tests and 3 tooling tests passed without live API calls.
Kernel and regression-program source matches the tested staged builds.
Final wrapper follow-ups were checked with rule-based demos (yes and no),
all 20 demonstration checks, and two further runs each of ask and approval
on 3 CPUs. Normal build images are left ready to run.

Raw matrix, final follow-up, host-test and demo records are committed under
`design/evidence/approval-fixes/`. The stage records identify the pre-commit
working tree used for this verification. Older evidence remains historical.

## Current-version regressions

Run `python3 tests/run-review-tests.py` after building the normal images.
It covers create/delete/modify, equal-size edits, unchanged content, declined
changes, combined categories, create-then-delete, nested paths, hard links,
and conflict rejection with current-public previews. The existing approval,
ask, conflict and namespace suites remain required checks. Older matrices and
spreadsheet D65 describe the previous saved-original implementation.

### Verification of this implementation (2026-11-14)

- Host: 41 agent tests and 3 verification-tool tests passed.
- Normal build: the new review regressions, approval, ask (2 CPUs), namespace
  (2 CPUs), lifecycle, durable commit, and conflict (3 CPUs, repeated twice)
  suites passed.
- Instrumented build: combined shadow suite (2 CPUs), failure/cleanup
  (2 CPUs), approval (3 CPUs), and commit crash-recovery checks passed.
- Normal images were rebuilt after instrumented checks for interactive demos.

The new review harness writes `agent/logs/review-regressions.jsonl`. Other
current-run raw and console logs are `/tmp/current-review-*.log` and
`/tmp/current-review-*.stdout`; these are local verification records. This is
not a rerun of the complete historical performance or correctness matrix.
