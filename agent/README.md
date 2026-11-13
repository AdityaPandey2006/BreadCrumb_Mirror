# Rule-based agent for x86 xv6

This course experiment is the first step toward **Externalization barriers for
speculative processes**. It uses this repository's x86 xv6, Python 3's standard
library, and QEMU. There is no LLM, API key, Python package installation, or kernel
modification.

For the optional Gemini API planner, follow [LLM_SETUP.md](LLM_SETUP.md).
The rule-based commands below remain available without a key or network access.

From the `xv6-public` directory:

```sh
make
python3 agent/agent.py list
python3 agent/agent.py read report.txt
python3 agent/agent.py summarize report.txt
python3 agent/agent.py
```

Interactive mode supports `list`, `read FILE`, `summarize FILE`,
`create FILE TEXT`, `delete FILE`, `write FILE TEXT`, `append FILE TEXT`,
`print TEXT`, and `quit`.
Summarization extracts the first sentence; it does not interpret instructions.
Each invocation boots a new guest with fixtures under `/demo`. Files are guest
files, not files shared with the host. The dummy repository is a directory of
text files; Git does not run inside xv6.

The repository's [`demo/`](../demo/) folder holds the starting demonstration
files (`report.txt`, `important.txt`, and `inbox.txt`). Before a session starts,
the agent validates those files and imports their exact bytes into the guest's
`/demo` directory. Add or edit files in the host folder before launching the
agent to change the demonstration inputs. Use regular files with lowercase
xv6 filenames of at most 14 characters and ASCII text of at most 60 bytes
per file, including newlines. Subdirectories and symlinks are refused.
Content is transferred as hex data, so punctuation and instruction-like text
are never interpreted as shell commands during import.

Reads, speculative writes, and newly created files operate on the guest copies
under `/demo`. They do not change the repository's host `demo/` folder. Guest
changes last for the current session only. Rebuild with `make xv6.img fs.img`
after updating the agent's guest helpers; editing demo text needs only a new
agent session.

To create a small file, start interactive mode and enter:

```text
agent> create note.txt Hello world
```

The agent runs the write through `specrun ask` inside one speculation. The
kernel displays its change report and asks whether to commit. Answer `y` to
publish the file or `n` to discard it. Ordinary console output remains buffered.
The agent checks the resulting file and its contents. You can then use
`read note.txt` in the same session. Existing filenames are refused.

Contents must be short plain ASCII text, with at most ten words and 60
characters; the combined filename and command must also fit xv6's shell input
limit, so longer text may be refused. Spaces are normalized and a newline is
added. Commas and the standalone words `into` and `pipe` are not supported.
The contents use one data block; filesystem metadata also uses shadow blocks.
The kernel enforces the eight-block limit. This command targets the small
`/demo` directory, rather than promising arbitrary filesystem layouts will fit.
All created files disappear when this disposable guest exits.

One-shot usage (prompts for approval by default):

```sh
python3 agent/agent.py "create note.txt Hello world"
```

For scripted tests only, `--answer y` or `--answer n` supplies the decision.

To demonstrate speculative deletion in the same interactive session:

```text
delete note.txt
```

The agent runs `specrun ask rm,/demo/note.txt`. The kernel reports the private
deletion and asks for approval. Answer `n`, then `read note.txt`: the file and
its contents remain. Run `delete note.txt` again and answer `y`, then `list`:
the file is gone. The agent verifies each result. Missing files, directories,
and paths outside `/demo` are refused. Only the guest copy is deleted; the
host demonstration folder stays unchanged.

### Writing, appending, and buffered printing

Build with `make xv6.img fs.img` to include `specedit`, then try these in
interactive mode (each line is a separate speculation and approval question):

```text
write report.txt Updated report
read report.txt
append report.txt Another entry
read report.txt
print Hello world
```

`write` replaces all contents of an existing file, including removing an old
suffix when the new text is shorter. Since xv6 has no truncate call, this helper
privately unlinks and recreates the file under the same name. It requires a
regular file with one hard link; inode identity and open-descriptor semantics
are not preserved as they would be with an in-place truncate operation. Use
these rules in the agent's controlled demonstration guest.

`append` preserves the original bytes and adds the text at the end. Both rules
add a newline after the supplied text, check the private result byte for byte,
and ask for approval. Answer `n` to preserve the original file or `y` to publish
the edit. Missing files are refused. The existing file and the final file must
each fit within 60 bytes, including newlines. Shadow-capacity or I/O failures
abort the whole edit.

`print` writes to the speculative console buffer. During review, the kernel
shows the held byte count. The message itself appears only after you answer
`y` and commit succeeds; answering `n` discards it. The review report and
post-abort status messages use their existing special/status output paths.

Text uses the same plain ASCII character allowlist as creation. Spaces and
commas are preserved for these rules. Text is hex-encoded for transfer rather
than interpreted as a shell command. The encoded command must fit the guest's
shell input limit, so practical text limits are shorter than 59 characters
(roughly 19-25 for file edits, depending on filename length). A too-long command
is refused before it runs. No LLM or API is used.

### Multiple tasks in one request

Rebuild with `make xv6.img fs.img` to include the `speccopy` guest helper, then
start `python3 agent/agent.py` and enter:

```text
read report.txt, create copy.txt, write report.txt-contents into copy.txt
```

Absolute `/demo/` paths are also accepted. This is a fixed three-step grammar;
repeat the same source and destination in the write step. The agent validates
the filenames and invokes `speccopy SOURCE DESTINATION`. The helper begins one
speculation, reads the source, creates the destination, writes the exact bytes,
and verifies the destination before asking for approval. These operations share
one speculation; they are not separate commits. Answer `y` to commit or `n` to
discard the new file. Then `read copy.txt` checks it in the same session.

The source must be a regular ASCII text file of at most **60 bytes**, including
newlines. Empty files are supported. Unlike `create FILE TEXT`, this copy
preserves spaces, tabs, newlines and punctuation exactly, and never interprets
file contents as commands. Missing sources, existing destinations and failed
reads/writes cause an abort. Size is checked before destination creation. One
data block suffices for the contents; metadata also counts toward the kernel's
eight-block cap. A capacity failure aborts the whole operation. The cap is not
eight blocks of text alone, and the small-file limit does not guarantee every
possible directory layout will fit.

The launcher finds `../build/qemu-system-i386`, then an installed QEMU on PATH.
Override this when moving the project to another machine:

```sh
python3 agent/agent.py --qemu /path/to/qemu-system-i386 list
```

Build images before launching; the agent never rebuilds them automatically. Do
not build or modify source images while the agent is copying them. Each run
copies both `fs.img` and `xv6.img` to a temporary directory, launches QEMU with
one CPU, no display, no monitor, and serial stdio, then removes the copies on
exit. Guest changes do not persist to your source images.

## Demonstrations

### Two competing committers

Rebuild with `make xv6.img fs.img`, start `python3 agent/agent.py`, then enter:

```text
demo conflict
```

The guest helper creates `/demo/conflict.txt` containing `ORIGINAL`. Two
independent owner processes begin separate speculations and stage different
edits to that same file block: `WINNER_A` and `WINNER_B`. A pipe handshake
ensures both writes are staged before either owner may ask to commit. The
outside parent also verifies that the public file still contains `ORIGINAL`.

A asks first. Answer **`y`**: A commits. B then asks for approval; answer **`y`**
again. B's stale commit is rejected despite user approval, and B aborts.
The demo checks that repeating B's commit
also fails (abort-only), then verifies the file contains only A's version.
Run `read conflict.txt` to see `WINNER_A`. This ordering is deliberate and
repeatable, rather than a random scheduling race.

If you decline A, B asks next. Approve B to publish `WINNER_B`, or decline both
to retain `ORIGINAL`. These paths demonstrate user decisions; the A-approved
path with both answers `y` demonstrates conflict rejection. Declining B after
approving A simply aborts B without attempting its commit. Existing `conflict.txt` files are never
overwritten: use `delete conflict.txt` and approve that deletion before
repeating the demo, or start a fresh session. The final file stays available
for inspection in the current guest session.

The same demo can be launched directly:

```sh
python3 agent/agent.py "demo conflict"
```

These explicitly selected scenarios authorize effects only in the disposable
guest. Interactive creation also authorizes a write, behind the speculation
barrier and with an explicit approval decision.

```sh
python3 agent/agent.py --log agent/logs/baseline.jsonl demo baseline
python3 agent/agent.py --log agent/logs/injection.jsonl demo injection
```

`demo baseline` reads the original important file, creates and reads a tentative
draft, deletes the important file, and prints `APPROVED`. It then abandons the
attempt and verifies that the deleted file is still missing. File operations
and console output have already escaped the attempted work. This is a baseline
failure demonstration, not an implementation of speculative execution or abort.

`demo injection` reads an inbox containing a malicious instruction. An explicitly
scripted vulnerable planner proposes deletion after that read. The dispatcher
allows the valid demo action and xv6 executes it. This demonstrates a semantic
mistake that schema validation alone cannot prevent. Normal read/summarize never
follows file instructions. No real model or general prompt-injection detection
is being simulated.

### Behind the externalization barrier (Step 10)

```sh
python3 agent/agent.py --log agent/logs/speculative.jsonl demo speculative
python3 agent/agent.py --log agent/logs/speculative-benign.jsonl demo speculative-benign
```

The same scripted planner runs, but every proposed effect action is validated
and then executed inside one speculation through the fixed template
`specrun keep /demo/important.txt STEP...` (guest utility `specrun.c`). The
guard is an explicit rule: the file the user did not ask to change must still
exist in the speculation's private view, otherwise the attempt is aborted.

* `demo speculative`: the injected deletion and the `APPROVED` message are
  held, the guard sees the deletion, the attempt aborts, and nothing escapes;
  `important.txt` is intact.
* `demo speculative-benign`: a draft is created and `APPROVED` printed; the
  guard passes, the attempt commits, and `APPROVED` appears only after the
  durable commit.

The barrier contains effects until that decision; it does not detect the
injection. Committing without the guard would publish the deletion (see
`bench/demo.py`, demonstration 7). Text inside a speculation may not contain
commas, and the whole command must fit xv6 sh's 100-byte line.

### The user decides (decision D63)

```sh
python3 agent/agent.py --log agent/logs/ask.jsonl demo ask
python3 agent/agent.py --log agent/logs/ask-benign.jsonl demo ask-benign
python3 agent/agent.py --answer n demo ask      # scripted answer, no prompt
```

The same proposed effects run inside one speculation through the fixed
template `specrun ask STEP...`. When every step has finished, the guest owner
calls `xask`. The kernel compares saved original and private shadow blocks,
freezes the attempt, prints its own change report directly
(the only output that skips the hold), and waits for one console line. The
agent shows the question here and asks you:

```
[speculation 1] specrun requested review
Kernel change report (saved original -> private):
... directory/inode changes, including delete important.txt ...
... changed-block counts and bounded before/after byte previews ...
... held console/pipe counts, with their payloads still hidden ...
Commit these changes? [y/n]
```

* `y`: the owner calls `xcommit`; the deletion is published and `APPROVED`
  appears only after the durable commit.
* `n`: `xcommit` is refused by the kernel, the owner aborts, and neither the
  deletion nor `APPROVED` ever escapes.

The answer comes only from `input()` on this host (or `--answer`); planner or
LLM output never reaches the guest console. The report is generated by the kernel. It catches same-size rewrites, lists
all changed blocks and shows bounded escaped byte previews. Full paths are not
reconstructed; file data is identified by inode/offset and directory changes
include names. If the report cannot fit, approval is refused. Other owners and
ordinary console readers wait while a question is active.

Every commit now needs a yes: old `commit` and `keep` routes cannot bypass the
review. `--answer y`/`n` supplies explicit scripted decisions for controlled demos;
normal runs ask at this terminal. See [approval-review.md](../design/approval-review.md).

The JSONL log records proposed/validated actions, serial chunks (including boot
and kernel output), fixed guest commands, observations, and verification results.
The default `agent/logs/latest.jsonl` is overwritten each invocation; use `--log`
with distinct filenames to preserve evidence. Empty output alone is never proof
that a mutation succeeded: the demos inspect the resulting file state.

Speculative effect text rejects commas and the exact words `into` and `pipe`,
because `specrun` treats them as operators. Text is limited to ten words so
the guest argument array cannot silently truncate it. Ordinary read-only
requests and the existing fixed demonstration text are unaffected.

Run the validator boundary tests with:

```sh
python3 -m unittest discover -s agent -v
```

Both demo commands above are also end-to-end checks against the actual guest;
they exit with an error if their expected file state is not observed.

## Bridge and limitations

The pipeline is request -> deterministic planner -> structured action validator
-> fixed guest command -> xv6 utility -> system call -> observation. Tools allow
only a single filename under `/demo`, maximum 14 characters, and reject shell
syntax. The bridge does not run a host shell. It paces serial input because the
teaching UART can lose bytes when flooded. Commands use a random completion
marker followed by the shell prompt, with a timeout and a 1 MiB response limit.
This bridge assumes the supplied fixtures contain ordinary text; arbitrary guest
programs could spoof framing. It is a teaching harness, not a hostile-guest RPC
protocol. xv6 shell utilities do not expose reliable exit codes here, so their
diagnostics are returned directly. A timeout stops the session rather than
retrying an operation whose outcome may be uncertain.

This tree's `>` does not truncate existing files and `>>` does not implement
normal append semantics. Demos create the draft on a fresh image. Fixture setup
uses fixed text and paths; the fixtures are created afresh each boot.

Later kernel tests need a guest utility that calls `xbegin()` and forks children
within the same speculation, then commits or aborts. Separate shell commands
cannot establish that shared process-tree context. Add outside-process readers,
console buffering tests, pipes inside/outside the speculation, conflicts, crash
tests, and shadow-capacity tests as the kernel mechanism becomes available.
Python validation is not the kernel externalization barrier.

## Version control

Keep the xv6 sources, license, `agent/`, documentation, and selected small test
fixtures in your own xv6 Git repository. QEMU is an external dependency; document
its version/path rather than checking in its full source tree. Generated images,
binaries, and routine logs should remain ignored. Preserve selected raw evidence
separately when preparing the project submission. This experiment is not intended
for upstream QEMU contribution.
