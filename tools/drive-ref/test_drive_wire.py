# SPDX-License-Identifier: CC0-1.0
"""Protect wire acceptance, source coverage, EOI and exact timing from false positives."""

import csv
from pathlib import Path
import tempfile
import unittest

import drive_wire as wire


class WireTest(unittest.TestCase):
    """Exercise the actual decoder with independently constructed composed-net changes."""

    def setUp(self):
        """Own a fresh trace path for every test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "wire.csv"

    def write_trace(self, changes):
        """Serialize explicit physical levels, retaining simultaneous-edge source order."""
        current = dict(atn=1, eoi=1, dav=1, nrfd=1, ndac=0, ifc=1, dio=190)
        with self.path.open("w", encoding="ascii") as stream:
            writer = csv.DictWriter(stream, fieldnames=[
                "sequence", "seconds", "event", "source", *wire.LINES])
            writer.writeheader()
            for sequence, (seconds, event, levels) in enumerate(changes, 1):
                current.update(levels)
                writer.writerow(dict(current, sequence=sequence, seconds=seconds,
                                     event=event, source="fixture"))

    def accepted(self):
        """Create one stable, EOI-marked byte accepted before DAV releases."""
        return [
            ("0", "initial", {"nrfd": 0}),
            ("0", "edge", {"nrfd": 1}),
            ("0.000000000000000001", "edge", {"dav": 0, "eoi": 0}),
            ("0.000000000000000003", "edge", {"ndac": 1}),
            ("0.000000000000000006", "edge", {"dav": 1}),
            ("0.000000000000000010", "edge", {"ndac": 0}),
            ("0.000000000000000010", "stop", {}),
        ]

    def test_acceptance_and_exact_sub_nanosecond_phases(self):
        """A DAV edge is counted only after NDAC acceptance and DAV completion."""
        self.write_trace(self.accepted())
        result = wire.decode_wire(self.path)
        self.assertTrue(result["coverage"]["complete"])
        self.assertEqual(result["aborted"], [])
        self.assertEqual(result["issues"], [])
        frame, = result["frames"]
        self.assertEqual((frame["byte"], frame["eoi"]), (65, True))
        self.assertEqual(frame["ready_to_dav_as"], 1)
        self.assertEqual(frame["dav_to_accept_as"], 2)
        self.assertEqual(frame["accept_to_release_as"], 3)
        self.assertEqual(frame["release_to_rearm_as"], 4)

    def test_dav_without_acceptance_is_not_a_received_byte(self):
        """Abandoning DAV while NDAC remains low cannot prove bytes or EOF."""
        changes = self.accepted()
        del changes[3]
        self.write_trace(changes)
        result = wire.decode_wire(self.path)
        self.assertEqual(result["frames"], [])
        self.assertEqual(len(result["aborted"]), 1)

    def test_atn_aborts_unaccepted_data(self):
        """An ATN interruption preserves a pending byte instead of accepting it."""
        self.write_trace([
            ("0", "initial", {}),
            ("1", "edge", {"dav": 0}),
            ("2", "edge", {"atn": 0}),
            ("3", "edge", {"ndac": 1}),
            ("4", "edge", {"dav": 1}),
            ("5", "stop", {}),
        ])
        result = wire.decode_wire(self.path)
        self.assertEqual(result["frames"], [])
        self.assertEqual(result["aborted"][0]["reason"], "ATN/IFC interruption")

    def test_eoi_change_before_acceptance_is_reported(self):
        """Data and EOI must remain tied to the accepted byte."""
        changes = self.accepted()
        changes.insert(3, ("0.000000000000000002", "edge", {"eoi": 1}))
        self.write_trace(changes)
        self.assertTrue(wire.decode_wire(self.path)["issues"])

    def test_stop_without_initial_or_final_coverage_fails(self):
        """Missing endpoints never prove that no more wire events occurred."""
        for changes in ([], self.accepted()[1:], self.accepted()[:-1]):
            with self.subTest(changes=changes):
                self.write_trace(changes)
                with self.assertRaisesRegex(ValueError, "coverage"):
                    wire.decode_wire(self.path)

    def test_sequence_gap_fails(self):
        """A lost trace row must not look like a longer handshake phase."""
        self.write_trace(self.accepted())
        text = self.path.read_text(encoding="ascii").replace("\n3,", "\n9,")
        self.path.write_text(text, encoding="ascii")
        with self.assertRaisesRegex(ValueError, "sequence"):
            wire.decode_wire(self.path)

    def test_clock_regression_fails(self):
        """Timestamp order is checked independently of sequence continuity."""
        changes = self.accepted()
        changes[-1] = ("0", "stop", {})
        self.write_trace(changes)
        with self.assertRaisesRegex(ValueError, "reversed"):
            wire.decode_wire(self.path)

    def test_stop_cannot_hide_an_edge(self):
        """The terminal snapshot must agree with the final observed net state."""
        changes = self.accepted()
        changes[-1] = (changes[-1][0], "stop", {"dio": 255})
        self.write_trace(changes)
        with self.assertRaisesRegex(ValueError, "coverage"):
            wire.decode_wire(self.path)

    def test_phase_labels_preserve_status_interruption(self):
        """Returning from interposed status resumes the original seed case."""
        frames = [{"dav_as": index * wire.ATTOSECONDS} for index in (1, 3, 5)]
        wire.label_cases(frames, [
            {"seconds": "0", "phase": "5"}, {"seconds": "2", "phase": "9"},
            {"seconds": "4", "phase": "10"},
        ])
        self.assertEqual([frame["case"] for frame in frames], ["seed", "interposed", "seed"])

    def test_command_phase_map_keeps_original_cases_unchanged(self):
        """Separate command scenarios without relabeling existing captured references."""
        frames = [{"dav_as": index * wire.ATTOSECONDS} for index in (1, 3)]
        wire.label_cases(frames, [{"seconds": "0", "phase": "25"},
                                 {"seconds": "2", "phase": "26"}],
                         {25: "initialize", 26: "reset"})
        self.assertEqual([frame["case"] for frame in frames], ["initialize", "reset"])
        self.assertEqual(wire.PHASE_CASES[23], "reset_status")

    def test_independent_capture_must_match_bytes_and_eoi(self):
        """Wrong EOI cannot be hidden behind matching payload bytes."""
        result = {"frames": [
            {"case": "seed", "direction": "drive", "byte": 65, "eoi": True}]}
        wire.check_wire_bytes(result, {"seed": {"bytes": [65], "kernal_st": [64]}})
        for expected in ({"bytes": [66], "kernal_st": [64]},
                         {"bytes": [65], "kernal_st": [0]}):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, "wire bytes/EOI"):
                    wire.check_wire_bytes(result, {"seed": expected})

    def test_invalid_reference_cannot_generate_stimulus(self):
        """Never replay an aborted frame as if it had been accepted."""
        self.write_trace(self.accepted())
        result = wire.decode_wire(self.path)
        wire.label_cases(result["frames"], [])
        result["aborted"].append({"reason": "interruption"})
        with self.assertRaisesRegex(ValueError, "cannot replay"):
            wire.write_replay(self.path, result, self.path.with_suffix(".txt"))

    def test_window_requires_proven_outer_coverage(self):
        """A controlled stimulus window excludes boot activity without losing its source."""
        self.write_trace(self.accepted())
        with self.assertRaisesRegex(ValueError, "outside complete source"):
            wire.decode_wire(self.path, start_as=11)
        result = wire.decode_wire(self.path, start_as=0)
        self.assertEqual(result["coverage"]["start_as"], 0)
        self.assertEqual(len(result["frames"]), 1)
        self.assertIsNone(result["frames"][0]["ready_to_dav_as"])

    def test_replay_uses_accepted_commands_and_atn_release_order(self):
        """Replay the original TALK boundary, not a reconstructed register-write stream."""
        self.write_trace([
            ("0", "initial", {"atn": 0, "nrfd": 0, "dio": 183}),
            ("0", "edge", {"nrfd": 1}),
            ("1", "edge", {"dav": 0}),
            ("2", "edge", {"ndac": 1}),
            ("3", "edge", {"dav": 1}),
            ("4", "edge", {"ndac": 0}),
            ("5", "edge", {"atn": 1, "dio": 190, "eoi": 0}),
            ("6", "edge", {"dav": 0}),
            ("7", "edge", {"ndac": 1}),
            ("8", "edge", {"dav": 1}),
            ("9", "edge", {"ndac": 0}),
            ("10", "stop", {}),
        ])
        result = wire.decode_wire(self.path)
        wire.label_cases(result["frames"], [])
        target = self.path.with_suffix(".txt")
        wire.write_replay(self.path, result, target)
        self.assertEqual(target.read_text(encoding="ascii").splitlines(), [
            "ieee-replay-v1", "setup H 72 1 0", "control A 0 0 0", "setup R 65 0 1"])

    def test_acceptance_before_atn_is_not_an_abort(self):
        """An already-acknowledged byte remains accepted if ATN precedes DAV release."""
        changes = self.accepted()
        changes.insert(4, ("0.000000000000000004", "edge", {"atn": 0}))
        self.write_trace(changes)
        result = wire.decode_wire(self.path)
        self.assertEqual(len(result["frames"]), 1)
        self.assertEqual(result["aborted"], [])


if __name__ == "__main__":
    unittest.main()
