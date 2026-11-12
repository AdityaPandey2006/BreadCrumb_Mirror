#!/usr/bin/env python3
"""Boot a disposable copy of an xv6 build and drive its serial console.

Shared by bench/run-bench.py and bench/demo.py. Every boot copies xv6.img
and fs.img from the build directory into a temporary directory, so the
build's own images are never modified.

Ad-hoc use:  python3 bench/guest.py BUILD_DIR "cmd 1" "cmd 2" ...
"""
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

DEFAULT_QEMU = str(Path.home() / "qemu/build/qemu-system-i386")


class Guest:
    def __init__(self, build, qemu=DEFAULT_QEMU, cpus=1, log=None, folder=None, copy=True):
        self.log = log
        self.pending = b""
        self.events = []          # (host monotonic time, line) for marked lines
        self.tmp = None
        if folder is None:
            self.tmp = tempfile.TemporaryDirectory(prefix="xv6-guest-")
            folder = Path(self.tmp.name)
        self.folder = Path(folder)
        if copy:
            for name in ("xv6.img", "fs.img"):
                shutil.copyfile(Path(build) / name, self.folder / name)
        cmd = [qemu, "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-smp", str(cpus), "-m", "512",
               "-drive", f"file={self.folder / 'xv6.img'},index=0,format=raw",
               "-drive", f"file={self.folder / 'fs.img'},index=1,format=raw"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.until(rb"\$ ", timeout=60)

    def _read(self, timeout):
        if select.select([self.proc.stdout], [], [], timeout)[0]:
            data = os.read(self.proc.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("QEMU exited")
            now = time.monotonic()
            if self.log:
                self.log.write(data)
                self.log.flush()
            # timestamp calibration markers as soon as they arrive
            for m in re.finditer(rb"(CAL[01][^\r\n]*)\r?\n", self.pending[-64:] + data):
                self.events.append((now, m.group(1).decode()))
            self.pending += data
            return True
        return False

    def until(self, pattern, timeout=600):
        deadline = time.monotonic() + timeout
        while True:
            m = re.search(pattern, self.pending)
            if m:
                out = self.pending[:m.start()]
                self.pending = self.pending[m.end():]
                return out.decode(errors="replace").replace("\r", "")
            left = deadline - time.monotonic()
            if left <= 0:
                raise RuntimeError(f"guest timeout waiting for {pattern!r}")
            self._read(left)

    def type(self, line):
        for byte in (line + "\n").encode():
            self.proc.stdin.write(bytes([byte]))
            self.proc.stdin.flush()
            time.sleep(0.003)

    def run(self, command, timeout=600):
        """Run one shell command; return its output (echoed command removed)."""
        marker = "DONE" + uuid.uuid4().hex[:10]
        line = f"{command}; echo {marker}"
        if len(line) >= 99:
            raise ValueError("xv6 shell line limit (100 bytes) exceeded: " + line)
        self.type(line)
        out = self.until(rb"(?:^|\n)" + marker.encode() + rb"\r?\n", timeout)
        self.until(rb"\$ ", 30)
        lines = out.split("\n")
        if lines and lines[0].strip() == line:
            lines = lines[1:]
        return "\n".join(lines)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()
        if self.tmp:
            self.tmp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()


if __name__ == "__main__":
    build, cmds = sys.argv[1], sys.argv[2:]
    with Guest(build) as g:
        for c in cmds:
            print(f"$ {c}")
            print(g.run(c))
