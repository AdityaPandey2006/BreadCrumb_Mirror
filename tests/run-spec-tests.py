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


class Guest:
    """One QEMU boot of the images in `folder`, logging all serial output."""

    def __init__(self, args, folder, log):
        self.log = log
        self.logpath = args.log
        self.pending = b""
        cmd = [args.qemu, "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-smp", str(args.cpus), "-m", "512",
               "-drive", f"file={folder/'xv6.img'},index=0,format=raw",
               "-drive", f"file={folder/'fs.img'},index=1,format=raw"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)

    def until(self, pattern, timeout=180):
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.pending)
            if match:
                out = self.pending[:match.start()]
                self.pending = self.pending[match.end():]
                return out.decode(errors="replace")
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
        output = self.until(rb"(?:^|\n)" + marker.encode() + rb"\r?\n")
        self.until(rb"\$ ")
        print(output, flush=True)
        if expected not in output or "FAIL:" in output or "panic:" in output:
            raise RuntimeError(f"{command} failed; inspect {self.logpath}")
        return output

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(1, 2, 3, 4), default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--qemu", default=str(ROOT.parent / "build/qemu-system-i386"))
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--suite", default="shadow",
                        choices=("shadow", "lifecycle", "files", "commit", "crash", "namespace", "namespace-crash", "console", "usertests"))
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xv6-spec-tests-") as tmp, args.log.open("wb") as log:
        folder = Path(tmp)
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
                run("spectest", "SPEC TESTS PASSED")
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
