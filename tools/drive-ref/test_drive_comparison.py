# SPDX-License-Identifier: CC0-1.0
"""Keep byte/EOI mismatches separate from unadjusted protocol-phase diagnostics."""

import unittest

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


if __name__ == "__main__":
    unittest.main()
