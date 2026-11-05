#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check CI-only container edits do not affect build provenance."""

import json
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class CiBuildTests(unittest.TestCase):
    """Execute the workflow's setup and build preamble in a clean checkout."""

    def test_mount_override_is_restored_before_configuration(self):
        """Consume the CI mount override, then recover clean build inputs."""
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        setup = re.search(
            r"shell: python\n        run: \|\n((?:          .*\n|\n)+)", workflow
        ).group(1)
        preamble = workflow.split("runCmd: |\n", 1)[1].split(
            "            cmake --preset default", 1
        )[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / ".devcontainer/devcontainer.json"
            config.parent.mkdir()
            original = (ROOT / ".devcontainer/devcontainer.json").read_text()
            config.write_text(original)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(
                ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm", "fixture"], cwd=root, check=True,
                capture_output=True,
            )
            subprocess.run(
                ["python3", "-c", textwrap.dedent(setup)], cwd=root, check=True,
            )
            self.assertNotIn("mounts", json.loads(config.read_text()))
            subprocess.run(
                ["bash", "-c", textwrap.dedent(preamble)], cwd=root, check=True,
            )
            self.assertEqual(config.read_text(), original)
            result = subprocess.run(
                ["git", "diff", "--quiet", "HEAD"], cwd=root, check=False,
            )
            self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
