#!/usr/bin/env python3
"""Rule planner and serial bridge for the course's x86 xv6 tree."""
import argparse
import json
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
DEMO_DIR = ROOT / "demo"


def demo_files(folder=DEMO_DIR):
    """Load bounded text fixtures; never turn their contents into shell code."""
    files = {}
    if not folder.is_dir():
        raise ValueError(f"Missing demonstration folder: {folder}")
    for path in sorted(folder.iterdir()):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Demo entries must be regular files: {path.name}")
        validate(action("read_file", path="/demo/" + path.name))
        with path.open("rb") as source:
            data = source.read(61)
        if len(data) > 60 or any(byte not in (9, 10) and not 32 <= byte <= 126 for byte in data):
            raise ValueError(f"Demo file {path.name} must contain at most 60 bytes of ASCII text")
        files[path.name] = data
    return files


def validate(action, destructive=False):
    """Validate structured actions before constructing guest shell commands."""
    schemas = {"list_files": set(), "read_file": {"path"},
               "summarize": {"path"}, "create_file": {"path", "text"},
               "delete_file": {"path"}, "print_text": {"text"}}
    if not isinstance(action, dict) or set(action) != {"tool", "args"}:
        raise ValueError("Expected exactly tool and args")
    tool, args = action["tool"], action["args"]
    if not isinstance(tool, str) or tool not in schemas:
        raise ValueError("Unknown tool")
    if not isinstance(args, dict) or set(args) != schemas[tool]:
        raise ValueError("Incorrect arguments")
    if any(not isinstance(v, str) for v in args.values()):
        raise ValueError("Arguments must be strings")
    if "path" in args and not re.fullmatch(r"/demo/[a-z][a-z0-9_.]{0,13}", args["path"]):
        raise ValueError("Use a single xv6 filename under /demo (maximum 14 characters)")
    if "text" in args and not re.fullmatch(r"[A-Za-z0-9 .,!?_-]{1,60}", args["text"]):
        raise ValueError("Text must be 1-60 plain ASCII characters without shell syntax")
    if tool in {"create_file", "delete_file", "print_text"} and not destructive:
        raise ValueError("Effect tools are available only in explicitly selected demos")
    return tool, args


def specrun_step(action):
    """Translate one validated effect action into a specrun STEP (Step 10).

    specrun takes one shell argument per step with words separated by commas,
    so commas and words used as specrun operators are rejected rather than
    interpreted as command structure.
    """
    tool, args = validate(action, destructive=True)
    if "," in args.get("text", ""):
        raise ValueError("Text inside a speculation cannot contain commas")
    words = args.get("text", "").split()
    if any(word in {"into", "pipe"} for word in words):
        raise ValueError("Text inside a speculation cannot contain the words into or pipe")
    if len(words) > 10:  # specrun MAXW=12: echo + ten words + terminating null
        raise ValueError("Text inside a speculation is limited to ten words")
    if tool == "delete_file":
        return "rm," + args["path"]
    if tool == "create_file":
        return "echo," + ",".join(words) + ",into," + args["path"]
    if tool == "print_text":
        return "echo," + ",".join(words)
    raise ValueError("Only effect tools can run inside a speculation")


def speculation_command(guard, actions):
    """Fixed template: specrun keep GUARD STEP... (commit only if GUARD survives).

    The guard is a rule-based policy check made by the owner on its own private
    view before deciding; the kernel barrier only contains the effects until
    then. Neither detects prompt injection.
    """
    validate(action("read_file", path=guard))
    if not actions:
        raise ValueError("Empty speculation")
    return "specrun keep " + guard + " " + " ".join(specrun_step(a) for a in actions)


def ask_command(actions):
    """Fixed template: specrun ask STEP... (the human decides, D63).

    The guest owner summarizes the private changes and asks through xask; the
    answer comes only from the human at this host (Bridge.command's ask
    callback), never from planner or LLM output.
    """
    if not actions:
        raise ValueError("Empty speculation")
    return "specrun ask " + " ".join(specrun_step(a) for a in actions)


def plan(request):
    # A fixed grammar, not a shell or a natural-language instruction evaluator.
    sequence = re.fullmatch(
        r"read (\S+),\s*create (\S+),\s*write (\S+)-contents into (\S+)", request)
    if sequence:
        source, destination, written_source, written_destination = sequence.groups()
        source = source if source.startswith("/") else "/demo/" + source
        destination = destination if destination.startswith("/") else "/demo/" + destination
        written_source = written_source if written_source.startswith("/") else "/demo/" + written_source
        written_destination = (written_destination if written_destination.startswith("/")
                               else "/demo/" + written_destination)
        if (source, destination) != (written_source, written_destination):
            raise ValueError("The write step must use the same source and destination as read/create")
        return action("copy_file", source=source, destination=destination)
    if request == "list":
        return {"tool": "list_files", "args": {}}
    for verb in ("write", "append", "print"):
        if request.startswith(verb + " "):
            parts = request.split(maxsplit=1 if verb == "print" else 2)
            if len(parts) != (2 if verb == "print" else 3):
                raise ValueError(f"Use {verb} " + ("TEXT" if verb == "print" else "FILE TEXT"))
            args = {"text": parts[-1]}
            if verb != "print":
                args["path"] = parts[1] if parts[1].startswith("/") else "/demo/" + parts[1]
            return action(verb + "_request", **args)
    if request.startswith("create "):
        parts = request.split(maxsplit=2)
        if len(parts) != 3 or not parts[2].strip():
            raise ValueError("Use create FILE TEXT")
        path = parts[1] if parts[1].startswith("/") else "/demo/" + parts[1]
        return action("create_file", path=path, text=parts[2])
    for prefix, tool in (("read ", "read_file"), ("summarize ", "summarize"),
                         ("delete ", "delete_file")):
        if request.startswith(prefix):
            path = request[len(prefix):].strip()
            if not path.startswith("/"):
                path = "/demo/" + path
            return {"tool": tool, "args": {"path": path}}
    raise ValueError("Use list, read, summarize, create, delete, write, append, print, or a demo")


def edit_command(proposed):
    verb = proposed["tool"].removesuffix("_request")
    args = proposed["args"]
    # Reuse the ordinary text/path schema, without exposing new LLM tools.
    validate(action("print_text" if verb == "print" else "create_file", **args), destructive=True)
    if not args["text"].strip() or len(args["text"].encode("ascii")) + 1 > 60:
        raise ValueError("Use nonempty text of at most 59 ASCII characters (plus a newline)")
    command = f"specedit {verb} " + (args["path"] + " " if verb != "print" else "")
    command += args["text"].encode("ascii").hex()
    if len(command) + len("; echo DONE") + 12 >= 99:
        raise ValueError("Text and filename exceed the xv6 shell limit; shorten the text or filename")
    return command


def checked_request(request):
    if request == "demo conflict":
        return action("conflict_demo")
    proposed = plan(request)
    if proposed["tool"] in {"write_request", "append_request", "print_request"}:
        edit_command(proposed)
        return proposed
    if proposed["tool"] == "copy_file":
        args = proposed["args"]
        for path in args.values():
            validate(action("read_file", path=path))
        if args["source"] == args["destination"]:
            raise ValueError("Source and destination must be different files")
        return proposed
    creating = proposed["tool"] == "create_file"
    validate(proposed, destructive=creating or proposed["tool"] == "delete_file")
    if creating:
        # Echo normalizes spaces and adds one newline. Even the text limit of
        # 60 bytes fits one data block; inode/bitmap/directory blocks also count
        # against the kernel's eight-block cap. The serial shell is tighter.
        if not proposed["args"]["text"].split():
            raise ValueError("File text must contain a non-space character")
        command = ask_command([proposed])
        if len(command) + len("; echo DONE") + 12 >= 99:
            raise ValueError("Text and filename exceed the xv6 shell limit; shorten the text or filename")
    return proposed


def execute_request(bridge, request):
    proposed = checked_request(request)
    if proposed["tool"] == "conflict_demo":
        print("Two independent speculations will edit /demo/conflict.txt.")
        print("Both stage their edits first. Answer y for A, then y for B: A wins and B is rejected.")
        bridge.record(proposed=proposed, validated=proposed)
        result = bridge.command("spconflictdemo")
        if "CONFLICT: refused:" in result:
            raise ValueError("Delete conflict.txt before repeating this demonstration")
        if "CONFLICT: VERIFIED" not in result or "CONFLICT: FAIL" in result:
            raise RuntimeError("Conflict demonstration failed; check the guest log and rebuild images")
        bridge.record(observation=result)
        return result
    if proposed["tool"] in {"write_request", "append_request", "print_request"}:
        bridge.record(proposed=proposed, validated=proposed)
        result = bridge.command(edit_command(proposed))
        if "SPECEDIT: committed:" not in result and "SPECEDIT: aborted:" not in result:
            raise RuntimeError("Edit did not complete; rebuild images to include specedit")
        bridge.record(observation=result)
        return result
    if proposed["tool"] == "copy_file":
        args = proposed["args"]
        bridge.record(proposed=proposed, validated=proposed,
                      steps=["read source", "create destination", "write exact source contents"])
        result = bridge.command(f"speccopy {args['source']} {args['destination']}")
        if "SPECCOPY: committed:" not in result and "SPECCOPY: aborted:" not in result:
            raise RuntimeError("Copy did not complete; ensure speccopy is built into fs.img")
        bridge.record(observation=result)
        return result
    if proposed["tool"] not in {"create_file", "delete_file"}:
        return bridge.execute(proposed)
    path = proposed["args"]["path"]
    name = path.rsplit("/", 1)[1]

    def exists():
        return any(line.split()[0] == name for line in
                   bridge.command("ls /demo").splitlines() if line.split())

    if proposed["tool"] == "delete_file":
        entries = [line.split() for line in bridge.command("ls /demo").splitlines()]
        entry = next((fields for fields in entries if fields and fields[0] == name), None)
        if entry is None:
            raise ValueError("File does not exist")
        if len(entry) < 2 or entry[1] != "2":
            raise ValueError("Delete supports regular files only")
        original = bridge.command("cat " + path)
        bridge.record(proposed=proposed, validated=proposed)
        result = bridge.command(ask_command([proposed]))
        committed = "committed: changes published;" in result
        if not committed and "aborted" not in result:
            raise RuntimeError("Deletion did not complete; check the specrun guest helper")
        if exists() == committed:
            raise RuntimeError("File deletion verification failed")
        if committed:
            result += f"\nVerified: {path} was deleted."
        else:
            if bridge.command("cat " + path) != original:
                raise RuntimeError("Aborted deletion changed the file contents")
            result += f"\nVerified: {path} and its contents were preserved."
        bridge.record(observation=result)
        return result

    # The disposable guest is controlled by this serial session. Refuse an
    # existing name: xv6 shell redirection does not truncate existing files.
    if exists():
        raise ValueError("File already exists; choose a new filename")
    bridge.record(proposed=proposed, validated=proposed)
    result = bridge.command(ask_command([proposed]))
    committed = "committed: changes published;" in result
    if exists() != committed:
        raise RuntimeError("File creation verification failed")
    if committed:
        contents = bridge.command("cat " + path)
        expected = " ".join(proposed["args"]["text"].split())
        if contents != expected:
            raise RuntimeError("Committed file contents did not match the requested text")
        result += f"\nVerified {path}: {contents}"
    else:
        result += f"\nVerified: {path} was not created."
    bridge.record(observation=result)
    return result


class Bridge:
    def __init__(self, qemu, log, timeout=20, approval=None):
        self.qemu, self.log, self.timeout = qemu, log, timeout
        self.approval = approval
        self.pending = b""
        self.proc = None
        self.temp = None

    def record(self, **event):
        self.log.write(json.dumps({"time": time.time(), **event}) + "\n")
        self.log.flush()

    def until(self, pattern, keep_match=False):
        deadline = time.monotonic() + self.timeout
        while True:
            match = re.search(pattern, self.pending)
            if match:
                output, self.pending = self.pending[:match.start()], self.pending[match.end():]
                output = output.decode("utf-8", errors="replace").replace("\r", "")
                if keep_match:
                    return output, match.group(0).decode("utf-8", errors="replace")
                return output
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Guest response timed out; session stopped")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if ready:
                data = os.read(self.proc.stdout.fileno(), 4096)
                if not data:
                    raise RuntimeError("QEMU exited before completing the request")
                self.record(serial=data.decode("utf-8", errors="replace"))
                self.pending += data
                if len(self.pending) > 1024 * 1024:
                    raise RuntimeError("Guest response exceeded 1 MiB")

    def __enter__(self):
        fixtures = demo_files()
        self.temp = tempfile.TemporaryDirectory(prefix="xv6-agent-")
        try:
            folder = Path(self.temp.name)
            for name in ("xv6.img", "fs.img"):
                shutil.copyfile(ROOT / name, folder / name)
            command = [self.qemu, "-display", "none", "-monitor", "none",
                       "-serial", "stdio", "-no-reboot", "-smp", "1", "-m", "512",
                       "-drive", f"file={folder / 'xv6.img'},index=0,format=raw",
                       "-drive", f"file={folder / 'fs.img'},index=1,format=raw"]
            self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.record(boot=self.until(rb"\$ "))
            self.command("mkdir /demo")
            for name, data in fixtures.items():
                chunks = [data[i:i+20].hex() for i in range(0, len(data), 20)] or ["-"]
                for chunk in chunks:
                    result = self.command(f"demoload /demo/{name} {chunk}")
                    if result != "DEMOLOAD: ok":
                        raise RuntimeError("Demo import failed; rebuild xv6.img and fs.img with make")
                self.record(demo_import=name, bytes=len(data))
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            self.proc.stdin.close()
            self.proc.stdout.close()
        if self.temp:
            self.temp.cleanup()

    def type(self, line):
        # The teaching UART can drop bytes if a host pipe floods its FIFO.
        for byte in (line + "\n").encode():
            self.proc.stdin.write(bytes([byte]))
            self.proc.stdin.flush()
            time.sleep(0.003)

    def command(self, command, ask=None):
        """Run a fixed-template command.

        ask, if given, is called with the text of each xask question and
        returns True to commit; it must get its answer from the human.
        """
        # This method is private to fixed templates; user text never becomes a command.
        marker = "DONE" + uuid.uuid4().hex[:12]
        line = f"{command}; echo {marker}"
        if len(line) >= 99:
            raise ValueError("Command exceeds xv6 shell input limit")
        self.type(line)
        # Consume the submitted line before recognizing the completion marker.
        # A file need not end with a newline, so the marker may follow its last
        # byte directly. Matching only at line starts would hang on such files.
        self.until(re.escape(line.encode()) + rb"\r?\n")
        done = marker.encode() + rb"\r?\n"
        prompt = rb"(?:^|\n)\[approval [1-9][0-9]*\] commit\? \[y/n\] "
        output = ""
        while True:
            chunk = self.until(prompt + b"|" + done, keep_match=True)
            text, matched = chunk
            output += text
            if marker in matched:
                break
            question = re.split(r"\[speculation \d+\] ", text)[-1].strip()
            decide = ask or self.approval or ask_human
            answer = "y" if decide(question) else "n"
            self.record(question=question, answer=answer)
            self.type(answer)
            output += f"commit? [y/n] {answer}"
            self.until(rb"\n")    # the guest's echo of the answer line
            output += "\n"
        self.until(rb"\$ ")
        result = "\n".join(output.splitlines())
        self.record(command=command, output=result)
        return result

    def execute(self, action, destructive=False):
        self.record(proposed=action)
        tool, args = validate(action, destructive)
        self.record(validated=action)
        if tool == "list_files":
            result = self.command("ls /demo")
        elif tool in {"read_file", "summarize"}:
            result = self.command("cat " + args["path"])
            if tool == "summarize" and "cat: cannot open" not in result:
                result = "Extractive summary: " + result.split(".")[0] + "."
        elif tool == "delete_file":
            result = self.command("rm " + args["path"])
        elif tool == "create_file":
            result = self.command(f"echo {args['text']} > {args['path']}")
        else:
            result = self.command("echo " + args["text"])
        self.record(observation=result)
        return result


def action(tool, **args):
    return {"tool": tool, "args": args}


def demo(bridge, injection=False):
    def run(tool, **args):
        result = bridge.execute(action(tool, **args), destructive=True)
        print(f"{tool}: {result or '(no output; verify state below)'}")
        return result

    original = run("read_file", path="/demo/important.txt")
    if "Original important data." not in original:
        raise RuntimeError("Fixture verification failed")
    if injection:
        run("read_file", path="/demo/inbox.txt")
        print("Scripted vulnerable rule: inbox read -> propose deletion. No LLM involved.")
    else:
        run("create_file", path="/demo/draft.txt", text="Tentative result.")
        if "Tentative result." not in run("read_file", path="/demo/draft.txt"):
            raise RuntimeError("Creation verification failed")
    run("delete_file", path="/demo/important.txt")
    run("print_text", text="APPROVED")
    print("Attempt abandoned by planner. No kernel abort mechanism exists yet.")
    run("list_files")
    missing = run("read_file", path="/demo/important.txt")
    if "cat: cannot open /demo/important.txt" not in missing:
        raise RuntimeError("Expected deletion was not observed")
    bridge.record(check="baseline deletion verified", passed=True)
    print("Verified: deletion survived abandonment; console output was already released.")


def demo_speculative(bridge, injection=True):
    """Step 10: the same effects as the baseline demos, run behind the barrier.

    The scripted vulnerable rule still proposes the injected deletion; nothing
    here detects the injection. The agent runs every proposed effect inside one
    speculation with a guard: files the user did not ask to change
    (/demo/important.txt) must still exist in the private view. The owner checks
    that before deciding, so the injected deletion is contained and discarded,
    while a benign attempt is committed and its output released.
    """
    def show(label, result):
        print(f"{label}: {result or '(no output)'}")
        return result

    show("read_file /demo/important.txt", bridge.execute(action("read_file", path="/demo/important.txt")))
    if injection:
        show("read_file /demo/inbox.txt", bridge.execute(action("read_file", path="/demo/inbox.txt")))
        print("Scripted vulnerable rule: inbox read -> propose deletion + APPROVED. No LLM involved.")
        proposed = [action("delete_file", path="/demo/important.txt"),
                    action("print_text", text="APPROVED")]
    else:
        # Short text: the whole specrun line must fit xv6 sh's 100-byte buffer.
        proposed = [action("create_file", path="/demo/draft.txt", text="Ok"),
                    action("print_text", text="APPROVED")]
    for a in proposed:
        bridge.record(proposed=a)
    command = speculation_command("/demo/important.txt", proposed)
    bridge.record(validated_speculation=command)
    result = show("speculation", bridge.command(command))
    after = show("read_file /demo/important.txt", bridge.execute(action("read_file", path="/demo/important.txt")))
    listing = show("list_files", bridge.execute(action("list_files")))
    if injection:
        ok = ("aborted by guard" in result and "APPROVED" not in result
              and "Original important data." in after)
        bridge.record(check="injected deletion contained and discarded", passed=ok)
        if not ok:
            raise RuntimeError("Speculative containment check failed")
        print("Verified: deletion and APPROVED were held, then discarded by abort; file intact.")
    else:
        if "aborted by user" in result:
            ok = "APPROVED" not in result and "draft.txt" not in listing
            bridge.record(check="benign attempt declined by human", passed=ok)
            if not ok:
                raise RuntimeError("Declined attempt leaked an effect")
            print("Verified: user declined; draft and held output discarded.")
        else:
            ok = "committed" in result and "APPROVED" in result and "draft.txt" in listing
            bridge.record(check="benign attempt committed and output released", passed=ok)
            if not ok:
                raise RuntimeError("Speculative commit check failed")
            print("Verified: draft published and APPROVED released only after durable commit.")


def ask_human(question):
    """The only source of a commit answer: the person at this terminal."""
    print("\nThe speculation is finished and frozen. The guest asks:")
    print("  " + question.replace("\n", "\n  "))
    while True:
        reply = input("Commit these changes? [y/n] ").strip().lower()
        if reply in {"y", "yes", "n", "no"}:
            return reply in {"y", "yes"}


def demo_speculative_ask(bridge, injection=True, answer=None):
    """D63: the same effects, but the human approves or rejects the commit.

    The proposed actions run inside one speculation; the guest owner then shows
    a summary of the private changes through xask and waits. Nothing is
    published and no held output is shown unless the human answers yes.
    answer ("y"/"n") replaces the interactive question for scripted runs.
    """
    def show(label, result):
        print(f"{label}: {result or '(no output)'}")
        return result

    def decide(question):
        if answer is None:
            return ask_human(question)
        print(f"\nGuest asks:\n  {question}\nScripted human answer: {answer}")
        return answer == "y"

    show("read_file /demo/important.txt", bridge.execute(action("read_file", path="/demo/important.txt")))
    if injection:
        show("read_file /demo/inbox.txt", bridge.execute(action("read_file", path="/demo/inbox.txt")))
        print("Scripted vulnerable rule: inbox read -> propose deletion + APPROVED. No LLM involved.")
        proposed = [action("delete_file", path="/demo/important.txt"),
                    action("print_text", text="APPROVED")]
    else:
        proposed = [action("create_file", path="/demo/draft.txt", text="Ok"),
                    action("print_text", text="APPROVED")]
    for a in proposed:
        bridge.record(proposed=a)
    command = ask_command(proposed)
    bridge.record(validated_speculation=command)
    result = show("speculation", bridge.command(command, ask=decide))
    after = show("read_file /demo/important.txt", bridge.execute(action("read_file", path="/demo/important.txt")))
    show("list_files", bridge.execute(action("list_files")))
    released = "APPROVED" in result.split("commit? [y/n]")[-1]
    if "committed" in result:
        ok = released
        print("Verified: approved changes published; held output released after the commit.")
    elif "aborted by user" in result:
        ok = not released and "Original important data." in after
        print("Verified: rejected attempt discarded; important.txt intact and APPROVED never shown.")
    else:
        ok = False
    bridge.record(check="human-approved speculation outcome", passed=ok)
    if not ok:
        raise RuntimeError("Speculative ask check failed")


DEMOS = {"demo baseline", "demo injection", "demo speculative", "demo speculative-benign",
         "demo ask", "demo ask-benign"}


def find_qemu():
    for candidate in (ROOT.parent / "build/qemu-system-i386",):
        if candidate.is_file():
            return str(candidate)
    return shutil.which("qemu-system-i386") or shutil.which("qemu-system-x86_64")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("request", nargs="*", help='e.g. "read report.txt" or "demo baseline"')
    parser.add_argument("--qemu", default=find_qemu())
    parser.add_argument("--log", type=Path, default=ROOT / "agent/logs/latest.jsonl")
    parser.add_argument("--answer", choices=("y", "n"),
                        help="explicit scripted approval for automated runs (default: ask the human)")
    opts = parser.parse_args()
    request = " ".join(opts.request)
    try:
        # Reject invalid one-shot requests before booting QEMU.
        if request and request not in DEMOS:
            checked_request(request)
        if not opts.qemu:
            raise ValueError("QEMU not found; specify --qemu /path/to/qemu-system-i386")
        for name in ("xv6.img", "fs.img"):
            if not (ROOT / name).is_file():
                raise ValueError(f"Missing {name}; run make in {ROOT}")
        opts.log.parent.mkdir(parents=True, exist_ok=True)
        with opts.log.open("w") as log, Bridge(
                opts.qemu, log,
                approval=(lambda question: opts.answer == "y") if opts.answer else None) as bridge:
            print(f"Disposable x86 xv6 guest ready. Log: {opts.log}")
            if request:
                if request in ("demo speculative", "demo speculative-benign"):
                    demo_speculative(bridge, request == "demo speculative")
                elif request in ("demo ask", "demo ask-benign"):
                    demo_speculative_ask(bridge, request == "demo ask", opts.answer)
                elif request == "demo conflict":
                    print(execute_request(bridge, request))
                elif request.startswith("demo "):
                    demo(bridge, request == "demo injection")
                else:
                    print(execute_request(bridge, request))
            else:
                print("Commands: list, read FILE, summarize FILE, create FILE TEXT, delete FILE, quit.")
                print("Also: write FILE TEXT, append FILE TEXT, print TEXT")
                print("Conflict handling: demo conflict")
                print("Multiple tasks: read SOURCE, create DEST, write SOURCE-contents into DEST")
                while True:
                    request = input("agent> ").strip()
                    if request in {"quit", "exit"}:
                        break
                    try:
                        print(execute_request(bridge, request))
                    except ValueError as exc:
                        bridge.record(rejected=request, reason=str(exc))
                        print(f"Rejected: {exc}")
        return 0
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"Error: {exc}")
        return 1
    except (KeyboardInterrupt, EOFError):
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
