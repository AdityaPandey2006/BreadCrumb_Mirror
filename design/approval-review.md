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

## Report the actual private block changes

The kernel builds the report from `shadow.orig` and `shadow.data`, after the
owner is alone and while the filesystem view gate stabilizes metadata. It does
not copy whole files or rely on paths supplied by the program. Every changed
block is listed, including same-size rewrites. The report identifies inode,
allocation, file-data and directory changes where possible. Directory entries
provide created/deleted names; other file data is identified by inode and
logical offset, not a reconstructed full path.

Before/after previews are escaped and limited to 24 bytes starting at the first
changed byte. The number and first/last offsets of changed bytes are also shown.
This is a block inventory with previews, not a complete textual diff. Inode
metadata includes types, sizes and link counts. Console and pipe payloads remain
hidden; only their buffered byte/storage counts are shown. A supplied xask note
is an escaped label, not the authoritative report.

Existing original/private images are reused. One temporary 4 KiB text page is
allocated per owner preparing/waiting for review; queued owners may each hold
one page. If the complete block inventory cannot fit, or allocation fails,
review is refused and commit cannot proceed. No truncated report is accepted.
Commit separately compares saved originals against current public blocks;
outside changes during review can therefore still cause a conflict after yes.

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

## Verification completed

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
