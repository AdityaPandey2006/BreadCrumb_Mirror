#!/usr/bin/env python3
"""Step 10 final demonstrations on a disposable copy of the production build.

    python3 bench/demo.py BUILD_DIR [--qemu PATH] [--transcript FILE]

Every demonstration uses dummy text files under /demo in a temporary copy of
the disk image; the build's own images are never modified. Each claim is
checked from OUTSIDE the speculation (an ordinary shell command or the raw
serial log) and the script stops on the first failed check. Exit status 0
means every demonstration behaved as documented.

Barriers contain effects until an explicit commit/abort decision. They do
not detect prompt injection and do not decide whether a request is
trustworthy; the "keep" guard below is a simple rule supplied by the caller.
"""
import argparse
import re
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest import Guest, DEFAULT_QEMU  # noqa: E402

FIXTURES = {"report.txt": "The server handled 100 requests. Five failed.",
            "important.txt": "Original important data.",
            "a.txt": "original a",
            "a2.txt": "public version"}


class Demo:
    def __init__(self, g, out):
        self.g, self.out, self.checks = g, out, 0

    def say(self, text=""):
        print(text, flush=True)
        self.out.write(text + "\n")

    def sh(self, cmd, timeout=120):
        res = self.g.run(cmd, timeout)
        self.say(f"$ {cmd}")
        for line in res.split("\n"):
            if line.strip():
                self.say(f"    {line}")
        return res

    def check(self, ok, what):
        self.checks += 1
        self.say(f"  [{'PASS' if ok else 'FAIL'}] {what}")
        if not ok:
            raise SystemExit(f"demonstration check failed: {what}")

    def section(self, title, note):
        self.say("\n" + "=" * 72 + f"\n{title}\n" + "=" * 72)
        self.say(note)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path)
    ap.add_argument("--qemu", default=DEFAULT_QEMU)
    ap.add_argument("--transcript", type=Path, default=Path("demo-transcript.txt"))
    ap.add_argument("--serial", type=Path, default=None)
    a = ap.parse_args()
    serial = (a.serial or a.transcript.with_suffix(".serial.txt")).open("wb")
    with a.transcript.open("w") as out, Guest(a.build, qemu=a.qemu, log=serial, scripted_approval=True) as g:
        d = Demo(g, out)
        d.say("Step 10 demonstrations: externalization barriers for speculative processes")
        d.say(f"build: {a.build}  (disposable image copy; dummy files under /demo)")
        d.sh("mkdir /demo")
        for name, text in FIXTURES.items():
            d.sh(f"echo {text} > /demo/{name}")

        d.section("1. Ordinary execution: effects are immediate and persistent",
                  "No speculation: the write and the deletion are visible at once and cannot be withdrawn.")
        d.sh("echo ORDINARY_NOTE > /demo/note.txt")
        d.check("ORDINARY_NOTE" in d.sh("cat /demo/note.txt"), "ordinary write visible immediately")
        d.sh("rm /demo/note.txt")
        d.check("cannot open" in d.sh("cat /demo/note.txt"), "ordinary deletion is immediate; nothing can undo it")

        d.section("2. Speculative changes stay private while the attempt is live",
                  "specrun runs in the background: it creates draft.txt, prints a line, then pauses "
                  "3 s before committing. An ordinary shell looks in the meantime.")
        g.type("specrun commit echo,PRIVATE_DRAFT,into,/demo/draft.txt echo,HELD_LINE_2 pause,300 &")
        g.until(rb"\$ ", 30)
        live_ls = d.sh("ls /demo")
        live_cat = d.sh("cat /demo/draft.txt")
        d.check("draft.txt" not in live_ls and "cannot open" in live_cat,
                "outside the speculation draft.txt does not exist yet")
        d.check("HELD_LINE_2" not in live_ls + live_cat, "the speculation's console line is held, not printed")
        tail = g.until(rb"SPECRUN: committed[^\n]*\n", 60)
        d.say("    (background) " + " | ".join(l for l in tail.split("\n") if l.strip()))
        d.check("HELD_LINE_2" in tail, "held line released when the attempt committed")
        d.check("PRIVATE_DRAFT" in d.sh("cat /demo/draft.txt"), "after commit the draft is public")

        d.section("3. Abort: original state preserved, pending output suppressed",
                  "The handout's case: a speculative child removes a, writes hi into b and prints, then aborts.")
        res = d.sh("specrun abort rm,/demo/a.txt echo,hi,into,/demo/b.txt echo,ABORT_SECRET_LINE")
        d.check("aborted" in res, "speculation aborted")
        d.check("original a" in d.sh("cat /demo/a.txt"), "a.txt still exists with its original contents")
        d.check("cannot open" in d.sh("cat /demo/b.txt"), "b.txt was never created publicly")

        d.section("4. Commit: filesystem changes published, then output released",
                  "Same steps with commit. Output is released only after the commit record is durable (D37).")
        res = d.sh("specrun commit rm,/demo/a.txt echo,hi,into,/demo/b.txt echo,COMMIT_LINE")
        d.check("COMMIT_LINE" in res and res.index("COMMIT_LINE") < res.index("SPECRUN: committed"),
                "held output appears when xcommit completes, before specrun's own report")
        d.check("cannot open" in d.sh("cat /demo/a.txt"), "a.txt removed")
        d.check("hi" in d.sh("cat /demo/b.txt"), "b.txt created with hi")

        d.section("5. Internal pipe keeps working inside a speculation",
                  "cat and wc are both members; the pipe between them is internal, so bytes flow at once "
                  "(no deadlock); only the final console output is held until commit.")
        ordinary = d.sh("cat /demo/report.txt | wc").strip()
        res = d.sh("specrun commit cat,/demo/report.txt,pipe,wc")
        # Approval text now precedes the pipeline's held output. Match the
        # actual wc result rather than assuming it is the first output line.
        counts = re.search(r"(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s*$", res)
        d.check(counts is not None and ordinary.split()[:3] == list(counts.groups()),
                "speculative pipeline gives the same counts as the ordinary pipeline")

        d.section("6a. Capacity limit follows the documented policy (D22/D25, R01)",
                  "Copying a ~60 KiB binary into a new file inside a speculation: the eight-block cap "
                  "(shared data and metadata) makes the write short. Nothing spills to "
                  "the public disk; the program sees the short write (cat reports an error).")
        res = d.sh("specrun abort cat,/usertests,into,/demo/big.bin")
        d.check("cannot open" in d.sh("cat /demo/big.bin"), "aborted oversized attempt left no file")
        res = d.sh("specrun commit cat,/usertests,into,/demo/big.bin")
        ls = d.sh("ls /demo/big.bin")
        size = int(ls.split()[-1]) if ls.split() and ls.split()[-1].isdigit() else -1
        # Parent and new-file inodes may occupy different inode blocks, so
        # the metadata footprint depends on the image layout. Count the file's
        # reported blocks and require that the shared eight-block cap was full.
        inode = ls.split()[-2] if len(ls.split()) >= 4 else ""
        data_blocks = re.findall(r"(?m)^Block \d+: file inode " + re.escape(inode) + r" at offset (\d+)$", res)
        d.check(0 < size <= 5 * 512 and size == len(data_blocks) * 512 and
                "Changed blocks: 8." in res,
                f"commit published only the prefix fitting the full 8-block cap (got {size} bytes)")
        d.say("  Policy note: committing after a short write publishes the written prefix (D25); a "
              "careful program checks the count and aborts instead.")

        d.section("6b. Conflict: first committer wins (D24/D45)",
                  "A background speculation creates c.txt and pauses; meanwhile an ordinary process "
                  "creates its own c.txt. The outside change is published first, so the speculation's "
                  "commit is refused and it aborts. Nothing of the losing attempt is published.")
        g.type("specrun commit echo,SPEC_VERSION,into,/demo/c.txt echo,LOSER_LINE pause,300 &")
        g.until(rb"\$ ", 30)
        d.sh("echo OUTSIDE_VERSION > /demo/c.txt")
        tail = g.until(rb"SPECRUN: commit refused[^\n]*\n", 60)
        d.say("    (background) " + " | ".join(l for l in tail.split("\n") if l.strip()))
        d.check("LOSER_LINE" not in tail, "loser's held output was discarded")
        d.check("OUTSIDE_VERSION" in d.sh("cat /demo/c.txt"), "public c.txt is the outside version")

        d.section("6c. Outside write to a privately copied block is refused (D20)",
                  "A speculation overwrites a2.txt and pauses; an ordinary write to the same block is "
                  "refused (the shell's echo ignores the error), and the speculation then commits.")
        g.type("specrun commit echo,SPEC_A2,into,/demo/a2.txt pause,300 &")
        g.until(rb"\$ ", 30)
        d.sh("echo OUTSIDER > /demo/a2.txt")
        d.check("public version" in d.sh("cat /demo/a2.txt"), "outside write had no effect while the copy exists")
        g.until(rb"SPECRUN: committed[^\n]*\n", 60)
        d.check("SPEC_A2" in d.sh("cat /demo/a2.txt"), "speculation committed its version")

        d.section("7. What the barrier does NOT do",
                  "The rule-based agent's injected deletion is contained only because the caller's "
                  "guard decides to abort (see: python3 agent/agent.py demo speculative). The kernel "
                  "does not recognise malicious instructions; an owner that commits a harmful attempt "
                  "publishes it.")
        res = d.sh("specrun commit rm,/demo/important.txt echo,INJECTED_AND_COMMITTED")
        d.check("cannot open" in d.sh("cat /demo/important.txt"),
                "an unguarded commit publishes the injected deletion: containment is not detection")
        d.say(f"\nAll {d.checks} demonstration checks passed.")
    serial.close()
    log = (a.serial or a.transcript.with_suffix(".serial.txt")).read_bytes().decode(errors="replace")
    for secret in ("ABORT_SECRET_LINE", "LOSER_LINE"):
        # the typed command line echoes the token once; it must never be OUTPUT
        if log.count(secret) > 1:
            raise SystemExit(f"FAIL: suppressed output {secret} appeared in the serial log")
    print("serial log check: suppressed tokens appear only in the echoed command lines")


if __name__ == "__main__":
    main()
