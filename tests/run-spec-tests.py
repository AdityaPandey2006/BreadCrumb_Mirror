#!/usr/bin/env python3
"""Run guest tests on disposable x86 images and retain the raw serial log.

The crash suite (SHADOW_TEST=1 builds only) boots, halts the guest at a named
point inside a speculative commit, then boots the SAME disk image copy again
(no rebuild or replacement of fs.img) and checks what log recovery produced.
"""
import argparse
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]

# crash point -> filesystem state recovery must produce (decision D26)
CRASH_POINTS = [
    (1, "old", "before any log block is written"),
    (2, "old", "log blocks written, commit record (log header) not written"),
    (3, "new", "commit record written, nothing installed home"),
    (4, "new", "first of three blocks installed home"),
]

# Step 10: crash points in the output-release window, after the durable
# point. Filesystem must recover the complete NEW state at every point.
# Console expectation: "none" = no held console byte reached the serial log
# before the crash; "prefix" = exactly the first 16 held bytes and not the
# rest. Pipe delivery is recorded (observed), not asserted, because the
# outside reader may or may not run before the halted machine stops.
OUTPUT_CRASH_POINTS = [
    (5, "none", "filesystem durable, no held output released"),
    (6, "none", "first held pipe record delivered, second not"),
    (7, "none", "pipe output released, console not yet"),
    (8, "prefix", "first 16 held console bytes emitted, rest not"),
]
CONSOLE_TOKEN = "CONSOLE_TOKEN_0123456789_END"
CONSOLE_PREFIX = CONSOLE_TOKEN[:16]

LEAK_TOKENS = ["LEAK_ABORT_SECRET_999", "LEAK_KILL_SECRET_888", "LEAK_ABORT_PIPE_SECRET_777",
               "LEAK_KILL_PIPE_SECRET_666", "LEAK_CONFLICT_CONSOLE_555", "LEAK_CONFLICT_PIPE_444",
               "LEAK_EXIT_PIPE_222", "LEAK_EXIT_CONSOLE_333", "LEAK_BLOCKED_ABORT_111",
               "LEAK_FULLPIPE_000", "LEAK_CONSFAIL_A", "LEAK_PIPEFAIL_B", "LEAK_CYCLE_ABORT"]


def assert_no_leaks(out):
    for token in LEAK_TOKENS:
        if token in out:
            raise RuntimeError(f"FAIL: leak detected: {token} appeared in guest output")


APPROVAL_PROMPT = rb"(?:^|\n)\[approval [1-9][0-9]*\] commit\? \[y/n\] "

class Guest:
    """One QEMU boot of the images in `folder`, logging all serial output."""

    def __init__(self, args, folder, log):
        self.log = log
        self.logpath = args.log
        self.pending = b""
        self.timeout = getattr(args, "timeout", 180)
        cmd = [args.qemu, "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-smp", str(args.cpus), "-m", "512",
               "-drive", f"file={folder/'xv6.img'},index=0,format=raw",
               "-drive", f"file={folder/'fs.img'},index=1,format=raw"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)

    def until(self, pattern, timeout=180):
        return self.until_match(pattern, timeout)[0]

    def until_match(self, pattern, timeout=180, auto_approve=True):
        deadline = time.monotonic() + timeout
        collected = b""
        while True:
            match = re.search(pattern, self.pending)
            approval = re.search(APPROVAL_PROMPT, self.pending) if auto_approve else None
            if approval and (not match or approval.start() < match.start()):
                collected += self.pending[:approval.end()]
                self.pending = self.pending[approval.end():]
                self.log.write(b"\n===== HARNESS: explicit scripted approval y =====\n")
                self.type("y")
                continue
            if match:
                out = self.pending[:match.start()]
                self.pending = self.pending[match.end():]
                return (collected + out).decode(errors="replace"), match.group(0)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"Guest timeout; inspect {self.logpath}")
            if select.select([self.proc.stdout], [], [], remaining)[0]:
                data = os.read(self.proc.stdout.fileno(), 4096)
                if not data:
                    raise RuntimeError(f"QEMU stopped; inspect {self.logpath}")
                self.log.write(data)
                self.log.flush()
                self.pending += data
                if len(self.pending) > 4 * 1024 * 1024:
                    raise RuntimeError("Guest output exceeded 4 MiB")

    def type(self, line):
        for byte in (line + "\n").encode():
            self.proc.stdin.write(bytes([byte]))
            self.proc.stdin.flush()
            time.sleep(0.003)

    def run(self, command, expected):
        marker = "TESTDONE" + uuid.uuid4().hex[:8]
        self.type(f"{command}; echo {marker}")
        output = self.until(rb"(?:^|\n)" + marker.encode() + rb"\r?\n", timeout=self.timeout)
        self.until(rb"\$ ")
        print(output, flush=True)
        if expected not in output or "FAIL:" in output or "panic:" in output:
            raise RuntimeError(f"{command} failed; inspect {self.logpath}")
        return output

    def run_answering(self, command, answers, expected):
        """Run command; answer each xask prompt by the tag in its question.

        answers maps a question prefix to "y", "n", None (leave unanswered),
        or (answer, token): type answer only once token has appeared in the
        guest output (an ordering point, not a delay).
        Returns (whole output, list of (question, answer)).
        """
        marker = "TESTDONE" + uuid.uuid4().hex[:8]
        self.type(f"{command}; echo {marker}")
        pattern = APPROVAL_PROMPT + rb"|(?:^|\n)" + marker.encode() + rb"\r?\n"
        output, asked = "", []
        while True:
            out, hit = self.until_match(pattern, timeout=self.timeout, auto_approve=False)
            output += out
            if marker.encode() in hit:
                break
            output += "commit? [y/n] "
            questions = re.findall(r"\[speculation \d+\] ([^\r\n]*)", out)
            question = questions[-1] if questions else ""
            answer = next((a for tag, a in answers.items() if question.startswith(tag)), "?")
            if answer == "?":
                raise RuntimeError(f"unexpected prompt {question!r}; inspect {self.logpath}")
            if isinstance(answer, tuple):
                # On several CPUs a user printf (one write per character) can be
                # split by the prompt, which the kernel prints in one locked piece.
                answer, token = answer
                while token not in strip_prompts(output):
                    output += self.until(rb"\n", timeout=self.timeout) + "\n"
            asked.append((question, answer))
            if answer is not None:
                self.type(answer)
        self.until(rb"\$ ")
        print(output, flush=True)
        if expected not in output or "FAIL:" in output or "panic:" in output:
            raise RuntimeError(f"{command} failed; inspect {self.logpath}")
        return output, asked

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()


def strip_prompts(text):
    """Remove single-line xask prompts (printed atomically by the kernel)."""
    return re.sub(r"\r?\n\[review (\d+) BEGIN\]\r?\n.*?\[approval \1\] commit\? \[y/n\] ", "", text, flags=re.S)


def note(log, text):
    log.write(f"\n===== HARNESS: {text} =====\n".encode())
    log.flush()
    print(text, flush=True)


def crash_suite(args, folder, log, namespace=False):
    for point, expect, meaning in CRASH_POINTS:
        for name in ("xv6.img", "fs.img"):
            shutil.copyfile(ROOT / name, folder / name)
        note(log, f"crash point {point} ({meaning}); expect complete {expect} state")
        guest = Guest(args, folder, log)
        try:
            guest.until(rb"\$ ")
            program = "spnamespace" if namespace else "spcommit"
            guest.run(f"{program} crashsetup", "NSSETUP OK" if namespace else "CRASHSETUP OK")
            guest.type(f"{program} crash {point}")
            out = guest.until(rb"SPEC CRASH POINT " + str(point).encode() + rb"\r?\n", timeout=120)
            guest.until(rb"panic: spec crash point", timeout=30)
            replied = "xcommit returned" in out
            note(log, f"guest halted at crash point {point}; caller received a reply: "
                      f"{'yes' if replied else 'no'}")
            if replied:
                raise RuntimeError("caller received a reply before the crash point")
        finally:
            guest.stop()
        note(log, "rebooting the SAME fs.img (no rebuild, no copy)")
        guest = Guest(args, folder, log)
        try:
            guest.until(rb"\$ ")
            guest.run(f"{program} crashverify {expect}", "NSVERIFY OK" if namespace else f"CRASHVERIFY OK {expect}")
        finally:
            guest.stop()
        note(log, f"PASS crash point {point}: recovered complete {expect} state")


def output_crash_suite(args, folder, log):
    """Crash after the durable point while held output is being released."""
    results = []
    for point, console, meaning in OUTPUT_CRASH_POINTS:
        for name in ("xv6.img", "fs.img"):
            shutil.copyfile(ROOT / name, folder / name)
        note(log, f"output crash point {point} ({meaning}); expect complete new filesystem, console {console}")
        guest = Guest(args, folder, log)
        try:
            guest.until(rb"\$ ")
            guest.run("spfailure outcrash setup", "OCSETUP OK")
            guest.type(f"spfailure outcrash {point}")
            out = guest.until(rb"SPEC CRASH POINT " + str(point).encode() + rb"\r?\n", timeout=120)
            out += guest.until(rb"panic: spec crash point", timeout=30)
            # Collect whatever else the halted machine printed (other CPUs).
            time.sleep(1)
            if select.select([guest.proc.stdout], [], [], 0.5)[0]:
                data = os.read(guest.proc.stdout.fileno(), 65536)
                log.write(data)
                out += data.decode(errors="replace")
        finally:
            guest.stop()
        flat = out.replace("\r", "")
        full = CONSOLE_TOKEN in flat
        prefix = CONSOLE_PREFIX in flat
        pipes = [t for t in ("PIPE_TOKEN_FIRST", "PIPE_TOKEN_SECOND") if t in flat]
        if "xcommit returned" in flat or "unexpected commit reply" in flat:
            raise RuntimeError("caller received a reply before the crash point")
        if console == "none" and prefix:
            raise RuntimeError(f"FAIL: held console bytes appeared before crash point {point}")
        if console == "prefix" and (not prefix or full):
            raise RuntimeError(f"FAIL: crash point {point} expected exactly a 16-byte console prefix")
        if point == 5 and pipes:
            raise RuntimeError("FAIL: held pipe bytes delivered before crash point 5")
        if point == 6 and "PIPE_TOKEN_SECOND" in pipes:
            raise RuntimeError("FAIL: second pipe record delivered before crash point 6")
        note(log, f"crash point {point}: console {'full' if full else 'prefix' if prefix else 'none'}; "
                  f"pipe tokens seen by the outside reader before halt: {pipes or 'none'}")
        note(log, "rebooting the SAME fs.img (no rebuild, no copy)")
        guest = Guest(args, folder, log)
        try:
            guest.until(rb"\$ ")
            after = guest.run("spfailure outcrash verify", "OCVERIFY OK new")
        finally:
            guest.stop()
        if CONSOLE_TOKEN in after or "PIPE_TOKEN" in after:
            raise RuntimeError("FAIL: held output reappeared after reboot")
        results.append((point, meaning, "new", 'full' if full else 'prefix' if prefix else 'none',
                        "+".join(pipes) or "none"))
        note(log, f"PASS output crash point {point}: recovered complete new state; "
                  "lost output was not replayed after reboot")
    note(log, "SUMMARY point | meaning | recovered fs | console before halt | pipe seen before halt")
    for row in results:
        note(log, "SUMMARY " + " | ".join(str(x) for x in row))


ASK_LEAKS = ["LEAK_ASK_NO_222", "LEAK_ASK_KILL_111", "LEAK_FROZEN_WRITE",
             "LEAK_ASK_CONFLICT_333", "ASKTEST R1", "ASKTEST R2", "ASKTEST A again"]


def ask_suite(guest):
    """D63: owner approval prompt, answered from the host like a human would."""
    out, asked = guest.run_answering("spasktest", {"ASKTEST A": "y", "ASKTEST B": "n",
                                                   "ASKTEST C": ("y", "RIVAL_COMMITTED"),
                                                   "ASKTEST E": None, "commit requested by owner": "y"},
                                     "SPASKTEST: ALL TESTS PASSED")
    for token in ASK_LEAKS:
        if token in out:
            raise RuntimeError(f"FAIL: {token} appeared in guest output")
    if [q for q, _ in asked if q.startswith("ASKTEST")][:3] != ["ASKTEST A", "ASKTEST B", "ASKTEST C"]:
        raise RuntimeError(f"FAIL: unexpected prompts {asked}")
    # C: the yes was typed after the rival's commit, and the commit still failed.
    plain = strip_prompts(out)
    if not plain.index("RIVAL_COMMITTED") < plain.index("C results"):
        raise RuntimeError("FAIL: conflict case answered before the rival committed")
    if "commit=-1 commit2=-1 abort=0" not in out:
        raise RuntimeError("FAIL: yes after a lost conflict did not fail cleanly")
    # The question is shown before commit; held output only after the yes.
    if not out.index("ASKTEST A") < out.index("ASK_HELD_A") < out.index("A results"):
        raise RuntimeError("FAIL: held output released before the question or after the result line")

    # specrun ask: the summary describes the private changes; y publishes.
    guest.run("echo keep > askG", "")
    out, asked = guest.run_answering("specrun ask echo,hi,into,askF rm,askG echo,HELD_SPECRUN_Y",
                                     {"specrun requested review": "y"}, "committed")
    question = asked[0][0] if asked else ""
    body = out.split("Kernel change report", 1)[-1]
    for needle in ("create askF (3 bytes)", "delete askG"):
        if needle not in body.split("commit? [y/n]")[0]:
            raise RuntimeError(f"FAIL: summary lacks '{needle}': {question!r}")
    if "HELD_SPECRUN_Y" in body.split("commit? [y/n]")[0]:
        raise RuntimeError("FAIL: held output released before the answer")
    if "HELD_SPECRUN_Y" not in body.split("commit? [y/n]")[1]:
        raise RuntimeError("FAIL: held output not released after yes")
    guest.run("cat askF", "hi")
    if "cannot open" not in guest.run("cat askG", ""):
        raise RuntimeError("FAIL: approved deletion not published")

    # n discards: file kept, held output never shown (only the echoed command line).
    guest.run("echo keep > askG", "")
    out, _ = guest.run_answering("specrun ask rm,askG echo,LEAK_SPECRUN_N",
                                 {"specrun requested review": "n"}, "aborted by user")
    if out.count("LEAK_SPECRUN_N") != 1:
        raise RuntimeError("FAIL: held output of a rejected attempt was released")
    guest.run("cat askG", "keep")
    guest.run("rm askF", "")
    guest.run("rm askG", "")


def approval_suite(guest):
    # The recorded yes/no belongs to each labelled request, not another owner.
    out, asked = guest.run_answering("spapproval", {"PAIR_A": "y", "PAIR_B": "n"}, "PAIR_DONE")
    if len(asked) != 2 or "A_RESULT_1" not in strip_prompts(out) or "B_RESULT_0" not in strip_prompts(out):
        raise RuntimeError("approval answers crossed between owners")
    if "HELD_PAIR_B" in out:
        raise RuntimeError("rejected owner's output leaked")
    out, _ = guest.run_answering("spapproval kill", {"PAIR_A": None, "PAIR_B": "y"}, "PAIR_DONE")
    if "B_RESULT_1" not in strip_prompts(out) or "HELD_PAIR_A" in out:
        raise RuntimeError("killed approval owner did not release the channel cleanly")

    # A competing ordinary read must not steal the answer. Its own input is
    # sent only AFTER the owner has successfully received yes and committed.
    marker = "READDONE" + uuid.uuid4().hex[:8]
    guest.type(f"spapproval reader; echo {marker}")
    out, _ = guest.until_match(APPROVAL_PROMPT, auto_approve=False)
    guest.type("y")
    out += guest.until(rb"READ_OWNER_OK\r?\n") + "READ_OWNER_OK\n"
    guest.type("z")
    out += guest.until(rb"(?:^|\n)"+marker.encode()+rb"\r?\n")
    guest.until(rb"\$ ")
    if "READER_Z_OK" not in out or "FAIL:" in out:
        raise RuntimeError("ordinary reader stole the approval answer")

    guest.run("echo OLD > same", "")
    out, _ = guest.run_answering("specrun ask echo,NEW,into,same", {"specrun requested review": "y"}, "committed")
    report = out.split("Kernel change report",1)[-1].split("commit? [y/n]",1)[0]
    if "before" not in report or "OLD" not in report or "NEW" not in report:
        raise RuntimeError("same-size rewrite missing from kernel report")
    guest.run("cat same", "NEW")
    guest.run("rm same", "")

    out, _ = guest.run_answering("spapproval direct", {"commit requested by owner": "n"}, "DIRECT_ABORT_OK")
    if "DIRECT_HELD_SECRET" in out:
        raise RuntimeError("direct commit bypassed a no answer")
    guest.run("cat directf", "cannot open")
    out, _ = guest.run_answering("spapproval direct", {"commit requested by owner": "y"}, "DIRECT_COMMIT_OK")
    if "DIRECT_HELD_SECRET" not in out.split("commit? [y/n]")[-1]:
        raise RuntimeError("direct commit did not release after approval")
    guest.run("cat directf", "NEW")
    guest.run("rm directf", "")
    # Generic run auto-answers only valid prompts. If an incomplete report
    # incorrectly asks and publishes, the guest prints FAIL instead.
    guest.run("spapproval overflow", "REPORT_OVERFLOW_REFUSED")
    print("APPROVAL REGRESSIONS PASSED", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(1, 2, 3, 4), default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--qemu", default=str(ROOT.parent / "build/qemu-system-i386"))
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--suite", default="shadow",
                        choices=("shadow", "lifecycle", "files", "commit", "crash", "namespace", "namespace-crash", "console", "pipe", "conflict", "usertests",
                                 "failure", "drainfull", "output-crash", "ask", "approval"))
    parser.add_argument("--timeout", type=int, default=180,
                        help="seconds to wait for one guest command (hang detection)")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xv6-spec-tests-") as tmp, args.log.open("wb") as log:
        folder = Path(tmp)
        if args.suite == "output-crash":
            for _ in range(args.repeat):
                output_crash_suite(args, folder, log)
            print(f"PASS: output-crash, {args.cpus} CPU(s); raw log: {args.log}")
            return
        if args.suite in ("crash", "namespace-crash"):
            for _ in range(args.repeat):
                crash_suite(args, folder, log, args.suite == "namespace-crash")
            print(f"PASS: crash, {args.cpus} CPU(s); raw log: {args.log}")
            return
        for name in ("xv6.img", "fs.img"):
            shutil.copyfile(ROOT / name, folder / name)
        guest = Guest(args, folder, log)
        run = guest.run
        try:
            guest.until(rb"\$ ")
            if args.suite == "shadow":
                for _ in range(args.repeat):
                    run("shadowtest", "SHADOW TESTS PASSED")
                    run("spfiletest", "SPFILE TESTS PASSED")
                    run("spcommit", "SPCOMMIT TESTS PASSED")
                    out = run("spconsoletest", "SPCONSOLETEST: ALL TESTS PASSED")
                    if "LEAK_ABORT_SECRET_999" in out:
                        raise RuntimeError("leak detected: LEAK_ABORT_SECRET_999 appeared in console output")
                    if "LEAK_KILL_SECRET_888" in out:
                        raise RuntimeError("leak detected: LEAK_KILL_SECRET_888 appeared in console output")
                    out_pipe = run("sppipetest", "SPPIPETEST: ALL TESTS PASSED")
                    if "LEAK_ABORT_PIPE_SECRET_777" in out_pipe:
                        raise RuntimeError("leak detected: LEAK_ABORT_PIPE_SECRET_777 appeared in output")
                    if "LEAK_KILL_PIPE_SECRET_666" in out_pipe:
                        raise RuntimeError("leak detected: LEAK_KILL_PIPE_SECRET_666 appeared in output")
                    out_conf = run("spconflict", "SPCONFLICT: ALL TESTS PASSED")
                    if "LEAK_CONFLICT_CONSOLE_555" in out_conf:
                        raise RuntimeError("leak detected: LEAK_CONFLICT_CONSOLE_555 appeared in output")
                    if "LEAK_CONFLICT_PIPE_444" in out_conf:
                        raise RuntimeError("leak detected: LEAK_CONFLICT_PIPE_444 appeared in output")
                run("spectest", "SPEC TESTS PASSED")
            elif args.suite == "conflict":
                for _ in range(args.repeat):
                    out = run("spconflict", "SPCONFLICT: ALL TESTS PASSED")
                    if "LEAK_CONFLICT_CONSOLE_555" in out:
                        raise RuntimeError("FAIL: LEAK_CONFLICT_CONSOLE_555 leaked to console on conflict!")
                    if "LEAK_CONFLICT_PIPE_444" in out:
                        raise RuntimeError("FAIL: LEAK_CONFLICT_PIPE_444 leaked to external pipe on conflict!")
                    print("Conflict handling and concurrency checks passed: first-committer-wins, abort-only, zero leakage confirmed.")
            elif args.suite == "pipe":
                for _ in range(args.repeat):
                    out = run("sppipetest", "SPPIPETEST: ALL TESTS PASSED")
                    if "LEAK_ABORT_PIPE_SECRET_777" in out:
                        raise RuntimeError("FAIL: LEAK_ABORT_PIPE_SECRET_777 leaked to pipe reader!")
                    if "LEAK_KILL_PIPE_SECRET_666" in out:
                        raise RuntimeError("FAIL: LEAK_KILL_PIPE_SECRET_666 leaked to pipe reader!")
                    if "COMMITTED: SPEC_PIPE_COMMIT_MSG" not in out:
                        raise RuntimeError("FAIL: committed external pipe message missing!")
                    if "DELIVERED: CHILD_EXTERNAL_PIPE_MSG-OWNER_EXTERNAL_PIPE_MSG" not in out:
                        raise RuntimeError("FAIL: delivered external pipe message missing!")
                    print("Pipe isolation and release checks passed: no leakage, ordered commit release confirmed.")
            elif args.suite == "console":
                for _ in range(args.repeat):
                    out = run("spconsoletest", "SPCONSOLETEST: ALL TESTS PASSED")
                    # Strict host-side observer checks for absence of leakage:
                    if "LEAK_ABORT_SECRET_999" in out:
                        raise RuntimeError("FAIL: LEAK_ABORT_SECRET_999 leaked to console!")
                    if "LEAK_KILL_SECRET_888" in out:
                        raise RuntimeError("FAIL: LEAK_KILL_SECRET_888 leaked to console!")
                    if "AAAAAAA" in out:
                        raise RuntimeError("FAIL: overflow bytes leaked to console during abort!")
                    # Check that committed messages appeared:
                    for needle in ("SPEC_COMMIT_MSG_FD1", "SPEC_COMMIT_MSG_FD2",
                                   "PARENT_START-CHILD_PART-PARENT_END",
                                   "DUP_WRITE_MSG-OPEN_WRITE_MSG",
                                   "EMPTY_COMMIT_OUTPUT",
                                   "CONSOLE_AFTER_FS_DURABLE"):
                        if needle not in out:
                            raise RuntimeError(f"FAIL: committed message '{needle}' missing from console output!")
                    print("Console isolation and release checks passed: no leakage, ordered commit release confirmed.")
            elif args.suite == "lifecycle":
                run("spectest", "SPEC TESTS PASSED")
                run("spfiletest", "SPFILE TESTS PASSED")
                run("spcommit", "SPCOMMIT TESTS PASSED")
                run("addtest", "add(3, 4) = 7")
                run("hello_test", "hello() returned 0")
                run("mytest", "Hey, I am trying xv6")
            elif args.suite == "files":
                for _ in range(args.repeat):
                    run("spfiletest", "SPFILE TESTS PASSED")
            elif args.suite == "commit":
                for _ in range(args.repeat):
                    run("spcommit", "SPCOMMIT TESTS PASSED")
            elif args.suite == "failure":
                for _ in range(args.repeat):
                    out = run("spfailure", "SPFAILURE TESTS PASSED")
                    assert_no_leaks(out)
                    for needle in ("CONSFAIL_OK_LINE", "F8 PIPEFAIL GOT PIPEFAIL_OK",) if "F8" in out else ():
                        if needle not in out:
                            raise RuntimeError(f"FAIL: expected '{needle}' in output")
            elif args.suite == "drainfull":
                for _ in range(args.repeat):
                    run("spfailure drainfull", "SPFAILURE DRAINFULL PASSED")
            elif args.suite == "approval":
                for _ in range(args.repeat):
                    approval_suite(guest)
            elif args.suite == "ask":
                for _ in range(args.repeat):
                    ask_suite(guest)
            elif args.suite == "namespace":
                for _ in range(args.repeat):
                    run("spnamespace", "NAMESPACE TESTS PASSED")
            else:
                run("usertests", "ALL TESTS PASSED")
            print(f"PASS: {args.suite}, {args.cpus} CPU(s); raw log: {args.log}")
        finally:
            guest.stop()


if __name__ == "__main__":
    main()
