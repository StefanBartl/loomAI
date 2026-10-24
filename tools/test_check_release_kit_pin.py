"""Tests of check-release-kit-pin.py: every way a pin or a workflow can be wrong is refused, and only actions on
the pinned full commit, under a tag that still names it, pass. The remote is never asked: `remote_commit` is
replaced.

    python -m unittest discover -s tools -p "test_*.py"
"""

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "check_release_kit_pin", Path(__file__).with_name("check-release-kit-pin.py")
)
pin = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pin)

SHA = "7f84b859421e8fa1b927c8c27b5aa85886db957f"
OTHER = "56df6b11ba66e1d9dd918a7284709bfb84e36dcd"
PIN_FILE = f"# a comment\ntag = v0.18.1\ncommit = {SHA}\n"


def flow(*refs, path="release-kit/publish"):
    return "jobs:\n  publish:\n    steps:\n" + "".join(
        f"      - uses: StefanBartl/appkit/{path}@{ref} # v0.18.1\n" for ref in refs
    )


class ReadPin(unittest.TestCase):
    def test_a_tag_and_a_commit_are_a_pin(self):
        self.assertEqual(pin.read_pin(PIN_FILE), ("v0.18.1", SHA))

    def test_everything_else_is_refused(self):
        for text in (
            "tag = v0.18.1\n",
            f"commit = {SHA}\n",
            f"tag = main\ncommit = {SHA}\n",
            "tag = v0.18.1\ncommit = abc123\n",
            f"tag v0.18.1\ncommit = {SHA}\n",
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                pin.read_pin(text)


class Workflows(unittest.TestCase):
    def test_the_pinned_commit_passes(self):
        steps, problems = pin.workflow_problems({"a.yml": flow(SHA, SHA)}, SHA)
        self.assertEqual((steps, problems), (2, []))

    def test_another_commit_fails_and_names_the_file(self):
        steps, problems = pin.workflow_problems({"release.yml": flow(OTHER)}, SHA)
        self.assertEqual(steps, 1)
        self.assertIn("release.yml", problems[0])
        self.assertIn(OTHER, problems[0])

    def test_a_tag_or_a_short_commit_fails(self):
        for ref in ("v0.18.1", "main", SHA[:7]):
            with self.subTest(ref=ref):
                _, problems = pin.workflow_problems({"a.yml": flow(ref)}, SHA)
                self.assertIn("not pinned by a full commit", problems[0])

    def test_every_spelling_of_a_step_is_read(self):
        for line in (
            f'      - uses: "StefanBartl/appkit/release-kit/publish@{OTHER}"\n',
            f"      - uses: stefanbartl/APPKIT/release-kit/publish@{OTHER}\n",
            f"      - uses: StefanBartl/appkit@{OTHER}\n",
            f"        uses: 'StefanBartl/appkit/release-kit/prepare@{OTHER}' # v0\n",
        ):
            with self.subTest(line=line):
                steps, problems = pin.workflow_problems({"a.yml": line}, SHA)
                self.assertEqual(steps, 1)
                self.assertTrue(problems, line)

    def test_other_actions_are_not_ours(self):
        text = "      - uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803\n" + flow(SHA)
        steps, problems = pin.workflow_problems({"a.yml": text}, SHA)
        self.assertEqual((steps, problems), (1, []))

    def test_no_step_at_all_is_no_pass(self):
        steps, problems = pin.workflow_problems({"a.yml": "name: x\n"}, SHA)
        self.assertEqual(steps, 0)
        self.assertIn("looked at nothing", problems[0])


class Main(unittest.TestCase):
    def run_main(self, workflows, remote=None, offline=False, pin_text=PIN_FILE):
        with tempfile.TemporaryDirectory() as dir:
            pin_path = os.path.join(dir, "release-kit.pin")
            with open(pin_path, "w", encoding="utf-8") as f:
                f.write(pin_text)
            flows = os.path.join(dir, "workflows")
            os.mkdir(flows)
            for name, text in workflows.items():
                with open(os.path.join(flows, name), "w", encoding="utf-8") as f:
                    f.write(text)
            argv = ["--pin", pin_path, "--workflows", flows] + (["--offline"] if offline else [])
            with mock.patch.object(pin, "remote_commit", side_effect=remote or [SHA]), mock.patch(
                "builtins.print"
            ) as said:
                code = pin.main(argv)
            words = " ".join(str(call.args[0]) for call in said.call_args_list if call.args)
            return code, words

    def test_a_tag_that_still_names_the_commit_passes(self):
        code, words = self.run_main({"a.yml": flow(SHA)})
        self.assertEqual(code, 0, words)

    def test_a_moved_tag_fails(self):
        self.assertEqual(self.run_main({"a.yml": flow(SHA)}, remote=[OTHER])[0], 1)

    def test_a_tag_that_is_gone_fails(self):
        self.assertEqual(self.run_main({"a.yml": flow(SHA)}, remote=[None])[0], 1)

    def test_a_check_that_cannot_ask_is_no_pass(self):
        code, _ = self.run_main({"a.yml": flow(SHA)}, remote=OSError("no network"))
        self.assertEqual(code, 2)

    def test_offline_asks_nobody(self):
        code, _ = self.run_main({"a.yml": flow(SHA)}, remote=OSError("must not be asked"), offline=True)
        self.assertEqual(code, 0)

    def test_a_workflow_on_another_commit_fails_before_the_remote_is_asked(self):
        code, words = self.run_main({"a.yml": flow(OTHER)}, remote=OSError("must not be asked"))
        self.assertEqual(code, 1, words)

    def test_a_broken_pin_file_is_not_a_pass(self):
        self.assertEqual(self.run_main({"a.yml": flow(SHA)}, pin_text="tag = nope\n")[0], 2)


class RemoteCommit(unittest.TestCase):
    def ls_remote(self, lines):
        result = mock.Mock(returncode=0, stdout=lines, stderr="")
        return mock.patch.object(pin.subprocess, "run", return_value=result)

    def test_the_commit_of_an_annotated_tag_is_the_peeled_one(self):
        out = f"{OTHER}\trefs/tags/v0.18.1\n{SHA}\trefs/tags/v0.18.1^{{}}\n"
        with self.ls_remote(out):
            self.assertEqual(pin.remote_commit("v0.18.1"), SHA)

    def test_a_lightweight_tag_names_its_commit_itself(self):
        with self.ls_remote(f"{SHA}\trefs/tags/v0.18.1\n"):
            self.assertEqual(pin.remote_commit("v0.18.1"), SHA)

    def test_no_such_tag_is_none(self):
        with self.ls_remote(""):
            self.assertIsNone(pin.remote_commit("v9.9.9"))


if __name__ == "__main__":
    unittest.main()
