#!/usr/bin/env python3
"""Fail when the release-kit actions of the workflows are not the commit tools/release-kit.pin names.

The workflows take the actions `prepare` and `publish` of appkit's release-kit by the full commit of a tag
(`uses: StefanBartl/appkit/release-kit/publish@<sha> # vX.Y.Z`). tools/release-kit.pin names that tag once,
together with the commit it named when it was taken. This checks that

  * the tag still names that commit (a tag that was moved would change the actions under the same name);
  * every `uses: StefanBartl/appkit/...@<ref>` of a workflow, however it is written (quoted, in another
    case, from the root of the repository), is pinned by that full commit;
  * at least one such line exists (a pin that looked at nothing is no pass).

    python tools/check-release-kit-pin.py            against GitHub (git ls-remote)
    python tools/check-release-kit-pin.py --offline  only the workflows against the pin file, no network

Exit 0: in step. Exit 1: drift, what is wrong is listed. Exit 2: the pin cannot be read, or appkit cannot be
reached (no answer is no pass).
"""

import argparse
import os
import re
import subprocess
import sys

REPO_URL = "https://github.com/StefanBartl/appkit"
PIN = "tools/release-kit.pin"
TAG = re.compile(r"^v\d+\.\d+\.\d+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
USES = re.compile(
    r"""^\s*-?\s*uses:\s*["']?StefanBartl/appkit(?:/(?P<path>[^@\s"']+))?@(?P<ref>[^\s"'#]+)""",
    re.M | re.I,
)


def read_pin(text):
    """(tag, commit) of the pin file, or raise ValueError saying what is wrong."""
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep:
            raise ValueError(f"not a 'key = value' line: {line}")
        values[key.strip()] = value.strip()
    tag, commit = values.get("tag", ""), values.get("commit", "")
    if not TAG.match(tag):
        raise ValueError(f"tag must be vX.Y.Z, got '{tag}'")
    if not COMMIT.match(commit):
        raise ValueError(f"commit must be a full commit id, got '{commit}'")
    return tag, commit


def read_workflows(folder):
    """The text of every workflow file in the folder (none when it is not there)."""
    texts = {}
    if os.path.isdir(folder):
        for entry in sorted(os.listdir(folder)):
            if entry.endswith((".yml", ".yaml")):
                with open(os.path.join(folder, entry), encoding="utf-8") as f:
                    texts[entry] = f.read()
    return texts


def workflow_problems(texts, commit):
    """(steps found, what is wrong with them): each step must be pinned by the full commit of the pin."""
    problems = []
    steps = 0
    for name, text in sorted(texts.items()):
        for match in USES.finditer(text):
            steps += 1
            ref = match.group("ref")
            where = f"{name}: StefanBartl/appkit/{match.group('path') or ''}@{ref}"
            if not COMMIT.match(ref):
                problems.append(f"{where}: not pinned by a full commit")
            elif ref != commit:
                problems.append(f"{where}: {PIN} names {commit}, one commit for every step")
    if steps == 0:
        problems.append("no 'uses: StefanBartl/appkit/...' step in the workflows: the pin looked at nothing")
    return steps, problems


def remote_commit(tag):
    """The commit the tag names now: the peeled commit of an annotated tag."""
    refs = [f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"]
    result = subprocess.run(
        ["git", "ls-remote", REPO_URL, *refs],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"git ls-remote failed: {result.stderr.strip()}")
    found = {}
    for line in result.stdout.splitlines():
        sha, ref = line.split("\t", 1)
        found[ref] = sha
    return found.get(refs[1]) or found.get(refs[0])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pin", default=PIN)
    parser.add_argument("--workflows", default=".github/workflows")
    parser.add_argument("--offline", action="store_true", help="do not ask GitHub where the tag points")
    args = parser.parse_args(argv)
    try:
        with open(args.pin, encoding="utf-8") as f:
            tag, commit = read_pin(f.read())
    except (OSError, ValueError) as err:
        print(f"cannot read {args.pin}: {err}")
        return 2
    steps, problems = workflow_problems(read_workflows(args.workflows), commit)
    if problems:
        for problem in problems:
            print(problem)
        return 1
    if not args.offline:
        try:
            now = remote_commit(tag)
        except (OSError, RuntimeError, subprocess.SubprocessError) as err:
            print(f"the check could not run: {err}")
            return 2
        if now is None:
            print(f"the tag {tag} is not in {REPO_URL}")
            return 1
        if now != commit:
            print(f"the tag {tag} names {now} now, {args.pin} pins {commit}: the tag was moved")
            return 1
    print(f"ok: {steps} step(s) of the workflows at {tag} = {commit[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
