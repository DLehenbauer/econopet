# SPDX-License-Identifier: CC0-1.0
"""Test verify.sh acquisition using a local Git repository and synthetic ROM.

Run the real shell wrapper from outside a temporary repository whose path
contains spaces. Check pinned cloning, file-URL downloads, cache reuse,
script-relative paths, output preservation, corrupt-input rejection, partial
download cleanup, argument handling, and propagation of verification failures.
Also require missing prerequisites to fail suite setup rather than skip tests.

The Python verifier is replaced by an asserting test double that checks its
arguments and writes a synthetic artifact. This tests acquisition and delegation,
not actual ROM assembly. Fixtures are removed after each test, with no external
network access or proprietary ROM inputs.

Suite setup fails and lists missing tools unless Bash, Git, curl, Python 3,
ACME, da65, ca65, ld65, and mktemp are available. The ROM tools are checked
by the wrapper but are not invoked here.

Run this suite from the repository root:

    python3 tools/drive-roms/test_verify_shell.py -v
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REQUIRED_TOOLS = (
    "bash", "git", "curl", "python3", "acme", "da65", "ca65", "ld65", "mktemp",
)


class ShellTests(unittest.TestCase):
    """Run the real shell wrapper without network access or proprietary inputs."""

    @classmethod
    def setUpClass(cls):
        """Fail before creating fixtures when any required tool is unavailable."""
        super().setUpClass()
        missing = [tool for tool in REQUIRED_TOOLS if shutil.which(tool) is None]
        if missing:
            raise RuntimeError("Missing required tools: " + ", ".join(missing))

    def setUp(self):
        """Create a local pinned repository, ROM download and asserting runner."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository with spaces"
        self.tools = self.repository / "tools" / "drive-roms"
        self.tools.mkdir(parents=True)
        self.script = self.tools / "verify.sh"
        shutil.copyfile(Path(__file__).with_name("verify.sh"), self.script)
        self.workspace = self.repository / ".tmp" / "ai"
        self.output = self.workspace / "drive-roms" / "out"
        self.download = self.workspace / "drive-roms" / "download" / "fixture.bin"
        self.source = self.workspace / "cbmsrc"
        self.origin = self.root / "origin"
        self.origin.mkdir()
        for directory in ("DOS_4040", "DOS_8250", "CONTROLLER_8250"):
            path = self.origin / directory / "fixture.asm"
            path.parent.mkdir()
            path.write_text("fixture source\n", encoding="ascii")
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        self.commit = self.git("rev-parse", "HEAD").stdout.strip()
        self.payload = self.root / "fixture.bin"
        self.payload.write_bytes(b"synthetic ROM")
        manifest = {
            "source": {"repository": str(self.origin), "commit": self.commit},
            "drive_roms": [{
                "part": "fixture",
                "size": self.payload.stat().st_size,
                "sha256": hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                "url": self.payload.as_uri(),
            }],
        }
        (self.tools / "drive-emulation-roms.json").write_text(
            json.dumps(manifest), encoding="ascii"
        )
        (self.tools / "verify.py").write_text(
            '''# SPDX-License-Identifier: CC0-1.0
"""Assert shell delegation and retain a synthetic compiled artifact."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
workspace = root / ".tmp" / "ai"
expected = [
    "--source-root", str(workspace / "cbmsrc"),
    "--rom-dir", str(workspace / "drive-roms" / "download"),
    "--manifest", str(Path(__file__).with_name("drive-emulation-roms.json")),
    "--output-dir", str(workspace / "drive-roms" / "out"),
]
assert sys.argv[1:] == expected, f"Unexpected runner arguments: {sys.argv[1:]}"
manifest = json.loads(Path(expected[5]).read_text(encoding="ascii"))
rom = (Path(expected[3]) / "fixture.bin").read_bytes()
assert hashlib.sha256(rom).hexdigest() == manifest["drive_roms"][0]["sha256"], "Wrong ROM"
output = Path(expected[7])
output.mkdir()
(output / "source.prg").write_bytes(b"synthetic assembled image")
if (root / "fail-verification").exists():
    raise SystemExit(1)
''', encoding="ascii"
        )

    def git(self, *arguments):
        """Operate only on the disposable local source fixture."""
        return subprocess.run(
            ["git", "-C", str(self.origin), *arguments],
            check=True, capture_output=True, text=True,
        )

    def run_script(self, *arguments):
        """Invoke from outside the repository to exercise script-relative paths."""
        return subprocess.run(
            ["bash", str(self.script), *arguments], cwd=self.root,
            capture_output=True, text=True, check=False,
        )

    def test_acquisition_output_and_cache_reuse(self):
        """Pin the clone, verify downloaded bytes and retain compiled output."""
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.download.read_bytes(), self.payload.read_bytes())
        self.assertEqual(
            subprocess.check_output(["git", "-C", str(self.source), "rev-parse", "HEAD"],
                                    text=True).strip(),
            self.commit,
        )
        self.assertEqual((self.output / "source.prg").read_bytes(), b"synthetic assembled image")
        self.assertFalse((self.output.parent / "src").exists())
        self.assertFalse(list(self.download.parent.glob("*.part.*")))

        # A second run needs no upstream repository or ROM download.
        self.output.rename(self.root / "prior-output")
        self.origin.rename(self.root / "unavailable-origin")
        self.payload.unlink()
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.output / "source.prg").is_file())

    def test_existing_output_is_preserved_before_acquisition(self):
        """Refuse to replace prior evidence or begin acquiring inputs."""
        self.output.mkdir(parents=True)
        marker = self.output / "source.prg"
        marker.write_bytes(b"prior compiled ROM")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Output already exists", result.stderr)
        self.assertEqual(marker.read_bytes(), b"prior compiled ROM")
        self.assertFalse(self.source.exists())
        self.assertFalse(self.download.exists())

    def test_wrong_download_is_rejected_and_partial_removed(self):
        """Never publish a hash-mismatched download or invoke reconstruction."""
        self.payload.write_bytes(b"corrupt")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ROM identity mismatch", result.stderr)
        self.assertFalse(self.download.exists())
        self.assertFalse(list(self.download.parent.glob("*.part.*")))
        self.assertFalse(self.output.exists())

    def test_dirty_source_and_corrupt_cache_are_preserved(self):
        """Reject changed inputs rather than resetting or replacing them."""
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.output.rename(self.root / "prior-output")
        dirty = self.source / "DOS_4040" / "fixture.asm"
        original = dirty.read_bytes()
        dirty.write_bytes(b"local edits")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be clean", result.stderr)
        self.assertEqual(dirty.read_bytes(), b"local edits")
        dirty.write_bytes(original)
        self.download.write_bytes(b"bad cache")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ROM identity mismatch", result.stderr)
        self.assertEqual(self.download.read_bytes(), b"bad cache")
        self.assertFalse(self.output.exists())

    def test_verification_failure_propagates_with_evidence(self):
        """Retain failed verification artifacts and return the runner failure."""
        (self.repository / "fail-verification").touch()
        result = self.run_script()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue((self.output / "source.prg").is_file())

    def test_missing_prerequisites_fail_suite_setup(self):
        """Return a failing test-process exit and name every missing prerequisite."""
        available = self.root / "available-tools"
        available.mkdir()
        for tool in REQUIRED_TOOLS:
            if tool not in ("acme", "ld65"):
                (available / tool).symlink_to(shutil.which(tool))
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "-v"],
            env={**os.environ, "PATH": str(available)},
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Missing required tools: acme, ld65", result.stderr)

    def test_help_and_unknown_arguments_do_not_acquire_inputs(self):
        """Document the output path and reject unsupported arguments without side effects."""
        result = self.run_script("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(".tmp/ai/drive-roms/out", result.stdout)
        result = self.run_script("--unknown")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.workspace.exists())


if __name__ == "__main__":
    unittest.main()
