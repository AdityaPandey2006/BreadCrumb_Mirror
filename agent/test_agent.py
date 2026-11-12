"""Boundary tests; run with python3 -m unittest discover -s agent -v."""
import unittest

from agent import action, plan, specrun_step, speculation_command, validate


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


if __name__ == "__main__":
    unittest.main()
