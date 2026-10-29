# SPDX-License-Identifier: CC0-1.0
"""Test verify.py orchestration using controlled child-tool reports and exits.

Check the eight-case verification plan, summary/report hashes, input
preservation, and refusal to overwrite evidence or generate output inside
input directories. Require the recorded 4040 negative-control result rather
than accepting arbitrary failures, and reject incomplete or unexpected results.
Also check that missing prerequisites fail before output is created.

Child processes and tool-availability checks are mocked. Temporary synthetic
inputs and reports are removed after each test. Only Python 3 is required,
with no downloads, assemblers, or proprietary ROM inputs. This suite tests
batch decisions and evidence handling, not actual ROM reconstruction.

Run this suite from the repository root:

    python3 tools/drive-roms/test_verify.py -v
"""

import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import verify as batch


class BatchTests(unittest.TestCase):
    """Exercise the full runner with controlled child-tool reports."""

    def setUp(self):
        """Create external input directories and a small independent manifest."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        for directory in ("DOS_4040", "DOS_8250", "CONTROLLER_8250"):
            (self.source / directory).mkdir(parents=True)
        self.roms = self.root / "roms"
        self.roms.mkdir()
        self.marker = self.roms / "input.bin"
        self.marker.write_bytes(b"untouched input")
        self.manifest = {
            "source": {"commit": batch.drive_rom_verify.SOURCE_COMMIT},
            "source_verification": {
                "DOS_8250": {"emitted_bytes": 6, "compared_bytes": 5, "matching_bytes": 5},
                "DOS_4040": {"emitted_bytes": 6, "compared_bytes": 5,
                             "matching_bytes": 1, "mismatch_count": 4},
            },
            "source_mappings": {
                name: {"source_digest": "synthetic-source"}
                for name in batch.SOURCE_MAPPINGS
            },
            "binary_mappings": {
                name: {"size": 5, "payload_sha256": hashlib.sha256(b"hello").hexdigest()}
                for name in batch.BINARY_MAPPINGS
            },
        }
        self.manifest_path = self.root / "manifest.json"
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="ascii")
        self.calls = []
        self.failure = None

    def child(self, command, **kwargs):
        """Produce tool-shaped evidence while rejecting unexpected invocations."""
        self.calls.append(command)
        self.assertIn(Path(command[1]).name, ("drive_rom_verify.py", "drive_rom_map.py"))
        output = Path(command[command.index("--output-dir") + 1])
        output.mkdir()
        if "--family" in command:
            family = command[command.index("--family") + 1]
            report_name = "comparison.json"
            negative = family == "4040"
            report = {
                "status": "mismatch" if negative else "emitted-bytes-match",
                "emitted_bytes": 6, "compared_bytes": 5,
                "matching_bytes": 1 if negative else 5,
                "mismatch_count": 4 if negative else 0,
                "source_commit": batch.drive_rom_verify.SOURCE_COMMIT,
                "source_digest": batch.drive_rom_verify.FAMILIES[family]["digest"],
            }
            code = 1 if negative else 0
        elif "--mapping" in command:
            report_name = "mapping.json"
            report = {
                "compared_bytes": 5, "matching_bytes": 5, "mismatch_count": 0,
                "source_commit": batch.drive_rom_verify.SOURCE_COMMIT,
                "source_digest": "synthetic-source",
            }
            code = 0
        else:
            self.assertIn("--binary-map", command)
            report_name = "mapping.json"
            report = {
                "payload_bytes": 5, "payload_sha256": hashlib.sha256(b"hello").hexdigest(),
                "roundtrip": "all payload bytes match",
            }
            code = 0
        if self.failure:
            code, report = self.failure(output.name, code, report)
        if report is not None:
            (output / report_name).write_text(json.dumps(report), encoding="ascii")
        kwargs["stdout"].write("synthetic tool output\n")
        return subprocess.CompletedProcess(command, code)

    def run_batch(self, output):
        """Invoke the CLI without installed assembler or real ROM dependencies."""
        with patch.object(batch.shutil, "which", return_value="/synthetic/tool"), \
                patch.object(batch.subprocess, "run", side_effect=self.child), \
                patch("builtins.print"):
            return batch.main([
                "--source-root", str(self.source), "--rom-dir", str(self.roms),
                "--manifest", str(self.manifest_path), "--output-dir", str(output),
            ])

    def test_complete_matrix_and_input_preservation(self):
        """Require all checks, record the negative control and retain tool identities."""
        output = self.root / "output"
        self.assertEqual(self.run_batch(output), 0)
        summary = json.loads((output / "summary.json").read_text())
        self.assertTrue(summary["passed"])
        self.assertEqual(len(self.calls), 8)
        negative = next(row for row in summary["results"] if row["case"] == "4040-source")
        self.assertEqual(negative["exit_code"], 1)
        self.assertTrue(negative["passed"])
        for row in summary["results"]:
            self.assertTrue((output / row["report"]).is_file())
            self.assertTrue((output / (row["case"] + ".log")).is_file())
            self.assertEqual(row["report_sha256"], batch.digest(output / row["report"]))
        self.assertEqual(summary["manifest_sha256"], batch.digest(self.manifest_path))
        self.assertEqual(set(summary["tools_sha256"]),
                         {"verify.py", "drive_rom_verify.py", "drive_rom_map.py"})
        self.assertEqual(self.marker.read_bytes(), b"untouched input")

    def test_arbitrary_failure_cannot_pass_negative_control(self):
        """Reject wrong exit, missing report and changed mismatch counts."""
        for kind in ("exit", "missing", "counts"):
            def failure(name, code, report):
                """Alter only the original-source negative control."""
                if name == "4040-source":
                    if kind == "exit":
                        return 2, report
                    if kind == "missing":
                        return 1, None
                    return 1, dict(report, matching_bytes=2, mismatch_count=3)
                return code, report

            self.failure = failure
            output = self.root / kind
            with self.subTest(kind=kind):
                self.assertEqual(self.run_batch(output), 1)
                summary = json.loads((output / "summary.json").read_text())
                self.assertFalse(summary["passed"])
                self.assertEqual([row["case"] for row in summary["results"] if not row["passed"]],
                                 ["4040-source"])

    def test_unexpected_positive_results_are_failures(self):
        """Do not accept nonzero exits, empty comparisons or altered payload hashes."""
        changes = (
            ("4040-revision2", 1, {}),
            ("4040-revision2", 0, {"compared_bytes": 0, "matching_bytes": 0}),
            ("4040-formatter", 0, {"payload_sha256": "0" * 64}),
        )
        for index, (target, exit_code, values) in enumerate(changes):
            def failure(name, code, report):
                """Change one otherwise successful child result."""
                return (exit_code, dict(report, **values)) if name == target else (code, report)

            self.failure = failure
            output = self.root / f"failure-{index}"
            with self.subTest(target=target, values=values):
                self.assertEqual(self.run_batch(output), 1)
                self.assertFalse(json.loads((output / "summary.json").read_text())["passed"])

    def test_existing_evidence_is_not_overwritten(self):
        """Refuse an existing output directory without executing any child."""
        output = self.root / "output"
        output.mkdir()
        marker = output / "retained"
        marker.write_bytes(b"original evidence")
        with self.assertRaises(FileExistsError):
            self.run_batch(output)
        self.assertFalse(self.calls)
        self.assertEqual(marker.read_bytes(), b"original evidence")

    def test_output_cannot_be_inside_supplied_inputs(self):
        """Do not generate derived material inside either external input tree."""
        for directory in (self.source, self.roms):
            output = directory / "output"
            with self.subTest(directory=directory), self.assertRaisesRegex(ValueError, "outside"):
                self.run_batch(output)
            self.assertFalse(output.exists())
        self.assertFalse(self.calls)

    def test_missing_prerequisites_fail_before_output(self):
        """Check unavailable tools and source directories before retaining evidence."""
        output = self.root / "output"
        with patch.object(batch.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "missing required tools"):
                batch.main([
                    "--source-root", str(self.source), "--rom-dir", str(self.roms),
                    "--manifest", str(self.manifest_path), "--output-dir", str(output),
                ])
        (self.source / "CONTROLLER_8250").rmdir()
        with self.assertRaisesRegex(ValueError, "missing input directory"):
            self.run_batch(output)
        self.assertFalse(output.exists())
        self.assertFalse(self.calls)


if __name__ == "__main__":
    unittest.main()
