"""Boundary tests; run with python3 -m unittest discover -s agent -v."""
import unittest
import tempfile
from pathlib import Path
from unittest.mock import Mock

from agent import (action, ask_command, checked_request, demo_files, execute_request, plan,
                   specrun_step, speculation_command, validate)


class DemoFolderTests(unittest.TestCase):
    def test_import_preserves_exact_text(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            content = b"two  spaces\ninto, pipe; $(id)\t!"
            (root / "sample.txt").write_bytes(content)
            (root / "empty.txt").write_bytes(b"")
            self.assertEqual(demo_files(root), {"empty.txt": b"", "sample.txt": content})

    def test_reject_oversize_binary_and_unsafe_entries(self):
        for name, content in (("big.txt", b"x" * 61), ("binary.txt", b"\x00"),
                              ("bad;rm", b"hi")):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / name).write_bytes(content)
                with self.assertRaises(ValueError):
                    demo_files(root)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "link").symlink_to("/etc/passwd")
            with self.assertRaises(ValueError):
                demo_files(root)


class CreateRequestTests(unittest.TestCase):
    def test_create_uses_approval_wrapper_and_verifies_contents(self):
        bridge = Mock()
        bridge.command.side_effect = ["report.txt 2 33 27", "committed: changes published;",
                                      "note.txt 2 36 12", "Hello world"]
        self.assertIn("Verified /demo/note.txt: Hello world",
                      execute_request(bridge, "create note.txt Hello world"))
        self.assertEqual(bridge.command.call_args_list[1].args,
                         ("specrun ask echo,Hello,world,into,/demo/note.txt",))

    def test_rejection_leaves_no_file(self):
        bridge = Mock()
        bridge.command.side_effect = ["", "aborted by user", ""]
        self.assertIn("was not created", execute_request(bridge, "create note.txt Hi"))

    def test_existing_file_is_never_overwritten(self):
        bridge = Mock()
        bridge.command.return_value = "report.txt 2 33 27"
        with self.assertRaisesRegex(ValueError, "already exists"):
            execute_request(bridge, "create report.txt Hi")
        self.assertEqual(bridge.command.call_count, 1)

    def test_invalid_creation_rejected_before_guest_execution(self):
        for request in ("create note.txt", "create ../escape Hi", "create note.txt x;rm",
                        "create note.txt into", "create note.txt a,b",
                        "create note.txt " + "x" * 60, "create note.txt    "):
            with self.subTest(request=request), self.assertRaises(ValueError):
                checked_request(request)

    def test_unexpected_commit_contents_fail_verification(self):
        bridge = Mock()
        bridge.command.side_effect = ["", "committed: changes published;",
                                      "note.txt 2 36 0", ""]
        with self.assertRaises(RuntimeError):
            execute_request(bridge, "create note.txt Hi")


class ConflictDemoTests(unittest.TestCase):
    def test_conflict_demo_uses_one_shared_guest(self):
        bridge = Mock()
        bridge.command.return_value = "CONFLICT: VERIFIED"
        self.assertIn("VERIFIED", execute_request(bridge, "demo conflict"))
        bridge.command.assert_called_once_with("spconflictdemo")

    def test_existing_fixture_is_refused(self):
        bridge = Mock()
        bridge.command.return_value = "CONFLICT: refused: existing file"
        with self.assertRaises(ValueError):
            execute_request(bridge, "demo conflict")

    def test_missing_helper_or_failed_checks_are_errors(self):
        for result in ("exec spconflictdemo failed", "CONFLICT: FAIL"):
            bridge = Mock()
            bridge.command.return_value = result
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                execute_request(bridge, "demo conflict")


class EditRequestTests(unittest.TestCase):
    def test_edits_use_one_reviewed_guest_command(self):
        for request, expected in (
                ("write report.txt New", "specedit write /demo/report.txt 4e6577"),
                ("append report.txt More", "specedit append /demo/report.txt 4d6f7265"),
                ("print Hello world", "specedit print 48656c6c6f20776f726c64")):
            bridge = Mock()
            bridge.command.return_value = "SPECEDIT: committed: done"
            with self.subTest(request=request):
                execute_request(bridge, request)
                bridge.command.assert_called_once_with(expected)

    def test_invalid_edits_never_execute(self):
        for request in ("write report.txt", "append /README x", "print hi;rm",
                        "write ../report.txt Hi", "print " + "x" * 60,
                        "append report.txt " + "x" * 50, "print    "):
            bridge = Mock()
            with self.subTest(request=request), self.assertRaises(ValueError):
                execute_request(bridge, request)
            bridge.command.assert_not_called()

    def test_missing_helper_is_not_success(self):
        bridge = Mock()
        bridge.command.return_value = "exec specedit failed"
        with self.assertRaises(RuntimeError):
            execute_request(bridge, "print Hi")


class DeleteRequestTests(unittest.TestCase):
    def test_approved_delete_uses_speculation_and_verifies_absence(self):
        bridge = Mock()
        bridge.command.side_effect = ["note.txt 2 40 6", "Hello",
                                      "SPECRUN: committed: changes published;", ""]
        self.assertIn("was deleted", execute_request(bridge, "delete note.txt"))
        self.assertEqual(bridge.command.call_args_list[2].args,
                         ("specrun ask rm,/demo/note.txt",))

    def test_aborted_delete_verifies_original_contents(self):
        bridge = Mock()
        bridge.command.side_effect = ["note.txt 2 40 6", "Hello", "SPECRUN: aborted by user",
                                      "note.txt 2 40 6", "Hello"]
        self.assertIn("contents were preserved", execute_request(bridge, "delete /demo/note.txt"))

    def test_missing_file_or_directory_never_reaches_delete(self):
        for listing in ("", "note.txt 1 40 32"):
            bridge = Mock()
            bridge.command.return_value = listing
            with self.subTest(listing=listing), self.assertRaises(ValueError):
                execute_request(bridge, "delete note.txt")
            self.assertEqual(bridge.command.call_count, 1)

    def test_unsafe_delete_paths_are_rejected(self):
        for request in ("delete /README", "delete ../report.txt", "delete note.txt;rm", "delete"):
            with self.subTest(request=request), self.assertRaises(ValueError):
                checked_request(request)

    def test_failed_delete_is_not_reported_as_successful(self):
        bridge = Mock()
        bridge.command.side_effect = ["note.txt 2 40 6", "Hello",
                                      "SPECRUN: committed: changes published;", "note.txt 2 40 6"]
        with self.assertRaises(RuntimeError):
            execute_request(bridge, "delete note.txt")


class CopyRequestTests(unittest.TestCase):
    def test_sequence_becomes_one_guest_transaction(self):
        bridge = Mock()
        bridge.command.return_value = "SPECCOPY: committed: copied 27 bytes"
        execute_request(bridge,
                        "read report.txt, create copy.txt, write /demo/report.txt-contents into copy.txt")
        bridge.command.assert_called_once_with("speccopy /demo/report.txt /demo/copy.txt")

    def test_inconsistent_or_unsafe_sequences_are_rejected(self):
        for request in (
                "read report.txt, create copy.txt, write inbox.txt-contents into copy.txt",
                "read report.txt, create copy.txt, write report.txt-contents into other.txt",
                "read /README, create copy.txt, write /README-contents into copy.txt",
                "read report.txt, create report.txt, write report.txt-contents into report.txt",
                "read report.txt, create a;rm, write report.txt-contents into a;rm"):
            with self.subTest(request=request), self.assertRaises(ValueError):
                checked_request(request)

    def test_missing_guest_helper_is_reported(self):
        bridge = Mock()
        bridge.command.return_value = "exec speccopy failed"
        with self.assertRaises(RuntimeError):
            execute_request(bridge,
                            "read report.txt, create copy.txt, write report.txt-contents into copy.txt")


class ValidationTests(unittest.TestCase):
    def test_read_only_requests(self):
        for request in ("list", "read report.txt", "summarize /demo/inbox.txt"):
            validate(plan(request))

    def test_guest_path_escape_and_shell_syntax(self):
        for path in ("/README", "/demo/../README", "/demo/a;rm", "/demo/a\nrm",
                     "/demo/sub/file", "/demo/abcdefghijklmnop"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate(action("read_file", path=path))

    def test_schema(self):
        for bad in (None, {}, action("unknown"), action("read_file", path=3),
                    action("list_files", extra="x"), {"tool": [], "args": {}}):
            with self.subTest(action=bad), self.assertRaises(ValueError):
                validate(bad)

    def test_effect_requires_demo(self):
        request = action("delete_file", path="/demo/important.txt")
        with self.assertRaises(ValueError):
            validate(request)
        validate(request, destructive=True)

    def test_demo_text_cannot_inject_commands(self):
        for text in ("x; rm /demo/a", "$(id)", "`id`", "a\nb", "x > /a"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                validate(action("print_text", text=text), destructive=True)


class SpeculationTemplateTests(unittest.TestCase):
    """Step 10: effects proposed by the planner are wrapped in a fixed specrun template."""

    def test_injected_plan_becomes_guarded_speculation(self):
        cmd = speculation_command("/demo/important.txt",
                                  [action("delete_file", path="/demo/important.txt"),
                                   action("print_text", text="APPROVED")])
        self.assertEqual(cmd, "specrun keep /demo/important.txt rm,/demo/important.txt echo,APPROVED")

    def test_create_text_words_joined_by_commas(self):
        self.assertEqual(specrun_step(action("create_file", path="/demo/draft.txt", text="Tentative result.")),
                         "echo,Tentative,result.,into,/demo/draft.txt")

    def test_rejects_unsafe_steps(self):
        for bad in (action("print_text", text="a,b"), action("read_file", path="/demo/report.txt"),
                    action("delete_file", path="/README"), action("print_text", text="x; rm /demo/a")):
            with self.subTest(action=bad), self.assertRaises(ValueError):
                speculation_command("/demo/important.txt", [bad])
        with self.assertRaises(ValueError):
            speculation_command("/README", [action("print_text", text="hi")])
        with self.assertRaises(ValueError):
            speculation_command("/demo/important.txt", [])

    def test_text_cannot_become_specrun_operators(self):
        for tool in ("print_text", "create_file"):
            for text in ("hello into unintended", "hello pipe rm important.txt",
                         "into", "pipe", "a a a a a a a a a a a"):
                args = {"text": text}
                if tool == "create_file":
                    args["path"] = "/demo/draft.txt"
                with self.subTest(tool=tool, text=text), self.assertRaises(ValueError):
                    speculation_command("/demo/important.txt", [action(tool, **args)])
        self.assertEqual(specrun_step(action("print_text", text="pipeline walkinto")),
                         "echo,pipeline,walkinto")

    def test_ask_template(self):
        """D63: the human decides; the template carries no guard."""
        self.assertEqual(ask_command([action("delete_file", path="/demo/important.txt"),
                                      action("print_text", text="APPROVED")]),
                         "specrun ask rm,/demo/important.txt echo,APPROVED")
        with self.assertRaises(ValueError):
            ask_command([])
        with self.assertRaises(ValueError):
            ask_command([action("print_text", text="x; rm /demo/a")])


if __name__ == "__main__":
    unittest.main()
