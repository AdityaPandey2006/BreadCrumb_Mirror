#!/usr/bin/env python3
"""Real-guest regressions for metadata + current-public-byte approval reports.

Build images first, then run: python3 tests/run-review-tests.py
No network, API calls or source-image writes; Bridge uses disposable images.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "agent"))
from agent import Bridge, ROOT, execute_request, find_qemu


def main():
    logfile = ROOT / "agent/logs/review-regressions.jsonl"
    logfile.parent.mkdir(parents=True, exist_ok=True)
    with logfile.open("w") as log, Bridge(find_qemu(), log, approval=lambda q: True) as b:
        for request, label in (
                ("create new.txt Hi", "Created: /demo/new.txt"),
                ("write report.txt New", "Modified: /demo/report.txt"),
                ("append report.txt More", "Modified: /demo/report.txt"),
                ("delete new.txt", "Deleted: /demo/new.txt")):
            out = execute_request(b, request)
            assert label in out, out
        print("PASS: created, deleted, replaced and extended files")

        b.command("echo ABC > /demo/same.txt")
        out = b.command("specrun ask echo,CAT,into,/demo/same.txt")
        assert "Modified: /demo/same.txt" in out and 'before "ABC' in out, out
        assert "current public -> private" in out, out
        out = b.command("specrun ask echo,CAT,into,/demo/same.txt")
        assert "Modified: /demo/same.txt" not in out and "Changed blocks: 0." in out, out
        print("PASS: equal-size edits detected; identical-byte writes omitted")

        b.approval = lambda q: False
        out = b.command("specrun ask echo,DOG,into,/demo/same.txt")
        assert "Modified: /demo/same.txt" in out, out
        assert b.command("cat /demo/same.txt") == "CAT"
        b.approval = lambda q: True
        print("PASS: rejecting a reported modification preserves contents")

        b.command("echo OLD > /demo/a")
        b.command("echo REMOVE > /demo/b")
        out = b.command("specrun ask echo,NEW,into,/demo/a echo,X,into,/demo/c rm,/demo/b")
        for label in ("Modified: /demo/a", "Created: /demo/c", "Deleted: /demo/b"):
            assert label in out, out
        print("PASS: one speculation reports all three categories")

        out = b.command("specrun ask echo,hi,into,/demo/gone rm,/demo/gone")
        assert "Created: /demo/gone" not in out and "Deleted: /demo/gone" not in out, out
        print("PASS: create-then-delete omitted from final file summary")

        b.command("mkdir /demo/sub")
        b.command("echo ABC > /demo/sub/f")
        out = b.command("specrun ask echo,CAT,into,/demo/sub/f")
        assert "Modified: /demo/sub/f" in out, out
        b.command("ln /demo/same.txt /demo/alias")
        out = b.command("specrun ask echo,FOX,into,/demo/same.txt")
        assert "Modified: /demo/same.txt" in out and "Modified: /demo/alias" in out, out
        print("PASS: nested paths and hard-link aliases")

        out = execute_request(b, "demo conflict")
        assert "B rejected: A already committed" in out, out
        assert 'before "A\\x0a' in out and 'after  "B\\x0a' in out, out
        assert b.command("cat /demo/conflict.txt") == "WINNER_A"
        print("PASS: current public previews plus generation-based conflict rejection")
    print(f"REVIEW REGRESSIONS PASSED; log: {logfile}")


if __name__ == "__main__":
    main()
