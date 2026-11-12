# Evidence: user approval before commit (xask, decision D63)

Recorded 12 November 2026 in WSL Ubuntu (gcc 15.2, Python 3.14), QEMU TCG
`qemu-system-i386`, 512 MiB. Builds were made from an LF copy of the working
tree (HEAD `8b1c3ab` plus the xask changes).

## matrix/ (all passed)

| File | What |
|---|---|
| `ask-test-cpu{1,2,3,4}-x8.txt` | `--suite ask`, 8 repeats per CPU count (raw serial log); `.console.txt` is the harness output |
| `ask-test-shadowbuild-cpu3-x3.txt` | `--suite ask` on the `SHADOW_TEST=1` build, 3 repeats |
| `{lifecycle,console,pipe,conflict,failure,files,commit,namespace}-normal-cpu{1,3}.txt` | existing suites on the xask kernel |
| `usertests-normal-cpu1.txt` | xv6 usertests on the xask kernel |
| `{shadow,crash,output-crash}-test-cpu3.txt` | `SHADOW_TEST=1` build: shadow suite and crash points 1-8 |

The `ask` suite covers: refusals (not speculative, live member, non-owner,
oversized question), A yes (freeze refuses write/open/fork/mkdir/unlink/read
and a second xask; commit publishes; held output after the question), B no
(xcommit refused, abort discards), C yes-then-conflict (rival commits first;
xcommit fails, abort-only, nothing leaks), E owner killed while waiting, and
`specrun ask` with y and n (summary contents, held output order, file state).

The regression and `SHADOW_TEST` shadow/crash logs were recorded before case C
was added to `spasktest.c`; the kernel was unchanged by that addition (only the
test program and harness changed).

## development/

`mutation-no-conflict-validation*.txt`: `shadow_publish` with the content
comparison disabled (`r = SP_OK`). Case C fails as intended:
`commit=0`, "xcommit after yes fails on conflict" and "public file holds the
rival's data only" both FAIL.

Harness fix found during development: on 2-3 CPUs the parent's
`RIVAL_COMMITTED` (user `printf`, one `write` per character) was split by the
kernel's prompt (one locked `cprintf`), so the harness waited for a token that
never appeared whole. `strip_prompts()` in `tests/run-spec-tests.py` removes the
prompt before matching. Those hung logs were overwritten by the rerun.

## demo/

`agent/agent.py demo ask` / `demo ask-benign` with scripted answers `--answer y`
and `--answer n` (text output and JSONL logs), one interactive run fed
`maybe` then `n` through `input()` (the invalid answer is asked again), and the
agent unit tests (20 tests).
