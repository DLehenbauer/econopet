# SPDX-License-Identifier: CC0-1.0
"""Keep byte/EOI mismatches separate from unadjusted protocol-phase diagnostics."""

import csv
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import compare_drive_reference as comparison
from compare_drive_reference import compare_frames, ordinary_file_prefix


class ComparisonTest(unittest.TestCase):
    """Compare independent frame dictionaries without an emulator or production adapter."""

    def frame(self, **changes):
        """Make one explicitly accepted original-ROM data transaction."""
        frame = dict(case="seed", byte=65, atn=False, eoi=True, direction="drive",
                     ready_to_dav_as=10, dav_to_accept_as=20, accept_to_release_as=30,
                     release_to_rearm_as=40)
        frame.update(changes)
        return frame

    def test_same_bytes_preserve_different_phase_times(self):
        """Timing differences are neither normalized away nor called physical parity."""
        differences, timings = compare_frames([self.frame()], [self.frame(ready_to_dav_as=40)])
        self.assertEqual(differences, [])
        self.assertEqual(timings[0]["ready_to_dav_as"], {"reference": 10, "candidate": 40})

    def test_eoi_and_status_byte_mismatches_are_exact(self):
        """A correct payload without the same final EOI is not equivalent."""
        differences, _ = compare_frames(
            [self.frame(case="startup")], [self.frame(byte=66, eoi=False)])
        self.assertEqual([row["field"] for row in differences], ["byte", "eoi"])
        self.assertTrue(all(row["case"] == "startup" for row in differences))

    def test_unobserved_timing_remains_unknown(self):
        """Missing readiness coverage cannot be replaced with an invented zero latency."""
        _, timings = compare_frames([self.frame()], [self.frame(ready_to_dav_as=None)])
        self.assertIsNone(timings[0]["ready_to_dav_as"]["candidate"])

    def test_write_differences_have_no_legacy_exemption(self):
        """Unimplemented writes remain exact differences, never accepted legacy behavior."""
        differences, _ = compare_frames(
            [self.frame(case="written_status")], [self.frame(byte=66)])
        self.assertEqual(differences, [
            {"frame": 0, "case": "written_status", "field": "byte",
             "reference": 65, "candidate": 66},
        ])

    def test_ndac_rearm_phase_is_compared(self):
        """Preserve receiver rearm timing independently of source DAV release."""
        differences, timings = compare_frames(
            [self.frame()], [self.frame(release_to_rearm_as=50)])
        self.assertEqual(differences, [])
        self.assertEqual(timings[0]["release_to_rearm_as"],
                         {"reference": 40, "candidate": 50})

    def test_ordinary_file_prefix_excludes_reset_command_itself(self):
        """UJ bytes tagged control must not sneak in before reset_status observations."""
        actions = ["write H 40 1 0", "readback R 13 0 1", "readback H 95 1 0",
                   "control A 0 0 0", "control H 40 1 0", "control H 255 1 0",
                   "control H 85 0 0", "control H 74 0 1", "reset_status R 55 0 0"]
        self.assertEqual(ordinary_file_prefix(actions), actions[:4])

    def test_ordinary_file_prefix_rejects_unfenced_readback(self):
        """A missing native termination cannot be manufactured by the comparator."""
        with self.assertRaises(ValueError):
            ordinary_file_prefix(["readback R 13 0 1", "control H 40 1 0"])


class ReceivedTest(unittest.TestCase):
    """Reject CPU input corruption even when composed-wire frames match."""

    def setUp(self):
        """Interleave host, ATN-release and receive actions with independent observations."""
        self.actions = ["setup H 72 1 0", "control A 0 0 0",
                        "startup R 65 0 0", "startup R 13 0 1", "write H 63 1 0"]
        self.rows = [
            dict(action="2", case="startup", expected="65", actual="65",
                 expected_eoi="0", actual_eoi="0"),
            dict(action="3", case="startup", expected="13", actual="13",
                 expected_eoi="1", actual_eoi="1"),
        ]

    def test_exact_receive_actions_and_selected_prefix(self):
        """Use action indices, not wire-frame indices, and respect prefix selection."""
        self.assertEqual(comparison.compare_received(self.actions, self.rows), [])
        self.assertEqual(comparison.compare_received(self.actions[:3], self.rows[:1]), [])
        self.assertEqual(comparison.compare_received(self.actions[:2], []), [])

    def test_corruption_order_and_provenance(self):
        """Do not trust recorded expectations or accept changed byte, EOI or action identity."""
        changes = (
            ("actual", "66"), ("actual_eoi", "1"), ("actual_eoi", "2"),
            ("action", "0"), ("case", "other"), ("expected", "66"),
            ("expected_eoi", "1"), ("actual", "invalid"), ("actual", None),
        )
        for field, value in changes:
            with self.subTest(field=field, value=value):
                rows = [dict(self.rows[0], **{field: value}), self.rows[1]]
                differences = comparison.compare_received(self.actions, rows)
                self.assertEqual([row["field"] for row in differences], [field])
                self.assertEqual(differences[0]["action"], 2)
        self.assertTrue(comparison.compare_received(self.actions, self.rows[::-1]))

    def test_missing_and_extra_receive_records(self):
        """Missing, duplicate and unexpected records cannot pass a zipped comparison."""
        for rows in ([], self.rows[:1], [*self.rows, self.rows[1]]):
            with self.subTest(count=len(rows)):
                differences = comparison.compare_received(self.actions, rows)
                self.assertEqual(differences[0]["field"], "received record count")

    def test_receive_mismatch_fails_cli_despite_matching_wire(self):
        """Retain receive differences in the report and return a failed comparison."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference"
            reference.mkdir()
            initial = reference / "initial.d64"
            initial.write_bytes(bytes(256))
            script = reference / "replay.txt"
            script.write_text("ieee-replay-v1\n" + "\n".join(self.actions) + "\n",
                              encoding="ascii")
            wire = reference / "ieee-wire.csv"
            wire.write_text("synthetic wire\n", encoding="ascii")
            frames = [
                dict(case=parts[0], byte=int(parts[2]), atn=bool(int(parts[3])),
                     eoi=bool(int(parts[4])), direction="drive" if parts[1] == "R" else "host")
                for parts in (action.split() for action in self.actions) if parts[1] != "A"
            ]
            original = {
                "model": "4040", "wire": {"frames": frames},
                "media": {"initial_sha256": comparison.sha256(initial),
                          "final_sha256": comparison.sha256(initial)},
                "artifacts": {path.name: comparison.sha256(path) for path in (wire, script)},
            }
            (reference / "result.json").write_text(json.dumps(original), encoding="ascii")
            tool = root / "replay-tool"
            tool.write_bytes(b"synthetic executable identity")
            output = root / "comparison"

            def replay(command, **kwargs):
                """Write complete replay evidence with only the CPU input byte corrupted."""
                candidate = Path(command[-1])
                candidate.mkdir()
                (candidate / "coverage.txt").write_text(
                    f"completed={len(self.actions)}\n", encoding="ascii"
                )
                (candidate / "final.d64").write_bytes(initial.read_bytes())
                with (candidate / "received.csv").open("w", encoding="ascii", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(self.rows[0]))
                    writer.writeheader()
                    writer.writerows([dict(self.rows[0], actual="66"), self.rows[1]])
                return subprocess.CompletedProcess(command, 0)

            with patch("sys.argv", [
                "compare_drive_reference.py", "--reference-dir", str(reference),
                "--output-dir", str(output), "--replay-tool", str(tool),
            ]), patch.object(comparison.subprocess, "run", side_effect=replay), \
                    patch.object(comparison.subprocess, "check_output", return_value="fixture"), \
                    patch.object(comparison, "decode_wire", return_value={
                        "frames": frames, "issues": [], "aborted": [],
                    }), patch("builtins.print"):
                with self.assertRaises(SystemExit) as failure:
                    comparison.main()
            self.assertEqual(failure.exception.code, 1)
            report = json.loads((output / "comparison.json").read_text())
            self.assertEqual(report["differences"], [])
            self.assertEqual(report["received_differences"][0]["field"], "actual")
            self.assertEqual(report["unexplained_differences"], report["received_differences"])


if __name__ == "__main__":
    unittest.main()
