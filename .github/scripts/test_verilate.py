# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check seed forwarding and execution-only simulator behavior."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class VerilateTests(unittest.TestCase):
    """Run the real wrapper against an isolated prebuilt simulator stand-in."""

    def setUp(self):
        """Create a disposable project with the real seed resolver and runner."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        repository = Path(__file__).resolve().parents[2]
        for name in ("verilate.sh", "seed.sh"):
            shutil.copyfile(repository / name, self.root / name)
            (self.root / name).chmod(0o755)
        (self.root / "gw/EconoPET").mkdir(parents=True)
        self.output = self.root / "run.json"
        self.model = self.root / "build/gw-test/bin/probe_tb_vl"
        self.model.parent.mkdir(parents=True)
        self.model.write_text("""#!/usr/bin/env python3
import json, os, sys
with open(os.environ['RUN_OUTPUT'], 'w') as output:
    json.dump({'args': sys.argv[1:], 'cwd': os.getcwd()}, output)
""")
        self.model.chmod(0o755)
        self.env = dict(os.environ, RUN_OUTPUT=str(self.output))
        for name in ("ECONOPET_TEST_SEED", "ECONOPET_TEST_SEED_FILE", "ECONOPET_MEDIA_DIR"):
            self.env.pop(name, None)

    def invoke(self, *args):
        """Execute the runner without a compiler or media directory."""
        return subprocess.run(["bash", str(self.root / "verilate.sh"), *args],
                              env=self.env, capture_output=True, text=True)

    def test_direct_name_and_default_reset(self):
        """Resolve the preset output directory without compiling anything."""
        result = self.invoke("probe_tb")
        self.assertEqual(result.returncode, 0, result.stderr)
        observed = json.loads(self.output.read_text())
        self.assertEqual(observed["args"], ["+verilator+rand+reset+0"])
        self.assertEqual(observed["cwd"], str(self.root / "gw/EconoPET"))

    def test_target_path_and_seed_override(self):
        """Forward CTest's explicit target path, reset mode, and shared seed."""
        seed_file = self.root / "test.seed"
        seed_file.write_text("456\n")
        self.env.update(ECONOPET_TEST_SEED_FILE=str(seed_file), ECONOPET_TEST_SEED="123")
        result = self.invoke(str(self.model), "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.output.read_text())["args"],
                         ["+verilator+rand+reset+2", "+verilator+seed+123", "+seed=123"])

    def test_seed_file(self):
        """Use the per-run seed when there is no explicit replay override."""
        seed_file = self.root / "test.seed"
        seed_file.write_text("456\n")
        self.env["ECONOPET_TEST_SEED_FILE"] = str(seed_file)
        result = self.invoke("probe_tb")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("+verilator+seed+456", json.loads(self.output.read_text())["args"])

    def test_missing_model(self):
        """Fail with build instructions rather than implicitly compiling."""
        result = self.invoke("missing_tb")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cmake --build --preset gw-test", result.stderr)
        self.assertFalse(self.output.exists())

    def test_invalid_seed(self):
        """Reject invalid replay seeds before executing a model."""
        self.env["ECONOPET_TEST_SEED"] = "0"
        result = self.invoke("probe_tb")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
