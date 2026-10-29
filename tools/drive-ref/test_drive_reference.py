# SPDX-License-Identifier: CC0-1.0
"""Protect reference isolation, bounded captures and native regression observations."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import drive_reference as reference


def native_observations(model="4040"):
    """Build the explicitly expected synthetic-case observations without an emulator."""
    version = b"2" if model == "4040" else b"2.7"
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    startup = b"73,CBM DOS V" + version + trailer
    data = {
        "startup": startup, "reset_status": startup,
        "consumed": b"00, OK" + trailer, "interposed": b"00, OK" + trailer,
        "written_status": b"00, OK" + trailer,
        "seed": reference.SEED, "onebyte": b"A\0\2A",
        "boundary254": bytes(range(254)), "boundary255": bytes(range(255)),
        "readback": reference.WRITTEN,
    }
    return {name: {"bytes": list(payload),
                   "kernal_st": [0] * (len(payload) - 1) + [64]}
            for name, payload in data.items()}


class MachineObservationTest(unittest.TestCase):
    """Distinguish full reference captures from bounded operator-checker logs."""

    def test_full_reference_rejects_capped_logs(self):
        """A bounded register prefix cannot prove full controller-I/O coverage."""
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_machine_observation("io_events=2000001\ntrace_complete=false\n")

    def test_operator_check_reports_bounded_log(self):
        """A completed operator checker may retain a separately labeled register prefix."""
        self.assertFalse(reference.check_machine_observation(
            "io_events=6000000\ntrace_complete=false\n", require_complete=False))

    def test_complete_log_is_retained(self):
        """Preserve complete observation provenance for either evidence mode."""
        self.assertTrue(reference.check_machine_observation("io_events=42\ntrace_complete=true\n"))

    def test_operator_check_still_requires_io(self):
        """Missing or zero I/O is not operator execution evidence."""
        for machine in ("trace_complete=true\n", "io_events=0\ntrace_complete=true\n"):
            with self.subTest(machine=machine), self.assertRaisesRegex(ValueError, "coverage"):
                reference.check_machine_observation(machine, require_complete=False)


class CaptureTest(unittest.TestCase):
    """Reject missing completion and invalid buffer counts instead of proving success."""

    def capture(self):
        """Encode a valid synthetic mailbox/capture using the shared layout."""
        capture = bytearray(0x1c00)
        mailbox = bytearray(32)
        mailbox[0] = reference.COMPLETE
        observations = native_observations()
        for name, (data, status, count, limit) in reference.LAYOUT.items():
            result = observations[name]
            size = len(result["bytes"])
            mailbox[count] = size & 255
            if limit > 255:
                mailbox[count + 1] = size >> 8
            begin = data - reference.CAPTURE_ADDRESS
            capture[begin:begin + size] = bytes(result["bytes"])
            begin = status - reference.CAPTURE_ADDRESS
            capture[begin:begin + size] = bytes(result["kernal_st"])
        return capture, mailbox

    def test_complete_capture_roundtrips(self):
        """Retain exact bytes, binary zeros and per-byte EOF observations."""
        capture, mailbox = self.capture()
        self.assertEqual(reference.decode_capture(capture, mailbox), native_observations())

    def test_incomplete_run_is_not_success(self):
        """An unfinished controller cannot supply reference expectations."""
        capture, mailbox = self.capture()
        mailbox[0] = 1
        with self.assertRaisesRegex(ValueError, "did not complete"):
            reference.decode_capture(capture, mailbox)

    def test_truncated_files_fail(self):
        """Partial artifact writes cannot be interpreted as complete observations."""
        capture, mailbox = self.capture()
        for content, counts in ((capture[:-1], mailbox), (capture, mailbox[:-1])):
            with self.subTest(capture=len(content), mailbox=len(counts)):
                with self.assertRaises(ValueError):
                    reference.decode_capture(content, counts)

    def test_invalid_counts_fail(self):
        """Reject both empty observations and traversal beyond the owned buffer."""
        for count in (0, 65):
            capture, mailbox = self.capture()
            mailbox[1] = count
            with self.subTest(count=count):
                with self.assertRaisesRegex(ValueError, "startup"):
                    reference.decode_capture(capture, mailbox)

    def test_wide_count_overflow_fails(self):
        """Reject a multi-sector count exceeding the fixed controller capture limit."""
        capture, mailbox = self.capture()
        mailbox[3:5] = (513).to_bytes(2, "little")
        with self.assertRaisesRegex(ValueError, "seed"):
            reference.decode_capture(capture, mailbox)


class ObservationTest(unittest.TestCase):
    """Require meaningful native distinctions rather than generic DOS success."""

    def test_expected_native_models(self):
        """Accept the documented model-specific startup and fixture observations."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_observations(model, native_observations(model))

    def test_one_byte_quirk_must_not_be_normalized(self):
        """The original four-byte stream is not equivalent to a corrected one-byte EOF."""
        observations = native_observations()
        observations["onebyte"] = {"bytes": [65], "kernal_st": [64]}
        with self.assertRaisesRegex(ValueError, "onebyte"):
            reference.check_observations("4040", observations)

    def test_timeout_or_early_eof_fails(self):
        """Neither a timer error nor misplaced EOI is a successful complete read."""
        for value in (2, 64):
            observations = native_observations()
            observations["seed"]["kernal_st"][0] = value
            with self.subTest(status=value):
                with self.assertRaisesRegex(ValueError, "EOF/error"):
                    reference.check_observations("4040", observations)

    def test_consumed_status_is_not_startup(self):
        """A second read must consume startup provenance rather than repeat it."""
        observations = native_observations()
        observations["consumed"] = observations["startup"]
        with self.assertRaisesRegex(ValueError, "consumed/write"):
            reference.check_observations("4040", observations)

    def test_dos27_drive_field_is_required(self):
        """The 8050/8250 trailing drive field must not be normalized away."""
        observations = native_observations("8050")
        observations["startup"] = native_observations()["startup"]
        with self.assertRaisesRegex(ValueError, "reset/status"):
            reference.check_observations("8050", observations)


class CommandTraceTest(unittest.TestCase):
    """Keep software I/O projections and missing observation coverage explicit."""

    def test_atn_and_inverted_dio(self):
        """Exclude data writes and bus releases from the ATN-tagged command projection."""
        text = (
            "seconds,direction,address,value\n"
            "1,write,e840,04\n2,write,e822,b7\n"
            "3,write,e840,00\n4,write,e822,b7\n"
            "5,write,e822,ff\n6,write,e822,9d\n"
            "7,read,e822,90\n8,write,e840,04\n9,write,e822,90\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.csv"
            path.write_text(text, encoding="ascii")
            self.assertEqual(reference.controller_commands(path),
                             [{"seconds": 4.0, "byte": 0x48},
                              {"seconds": 6.0, "byte": 0x62}])

    def test_interposed_status_and_source_resume(self):
        """Observe source, status, then source selection across the interposed phase."""
        commands = [
            {"seconds": 1.0, "byte": 0x48}, {"seconds": 1.1, "byte": 0x62},
            {"seconds": 2.1, "byte": 0x48}, {"seconds": 2.2, "byte": 0x6f},
            {"seconds": 4.0, "byte": 0x48}, {"seconds": 4.1, "byte": 0x62},
        ]
        phases = [{"seconds": "0", "phase": "5"}, {"seconds": "2", "phase": "9"},
                  {"seconds": "3", "phase": "10"}, {"seconds": "5", "phase": "6"}]
        reference.check_interruption(commands, phases)
        with self.assertRaisesRegex(ValueError, "not observed"):
            reference.check_interruption(commands[:-2], phases)
        commands[-2]["seconds"], commands[-1]["seconds"] = 6.0, 6.1
        with self.assertRaisesRegex(ValueError, "not observed"):
            reference.check_interruption(commands, phases)

    def test_missing_or_reversed_phase_coverage_fails(self):
        """A trace window with missing endpoints cannot prove interruption behavior."""
        for phases in ([], [{"seconds": "2", "phase": "9"}],
                       [{"seconds": "3", "phase": "9"}, {"seconds": "2", "phase": "10"}]):
            with self.subTest(phases=phases):
                with self.assertRaisesRegex(ValueError, "phase coverage"):
                    reference.check_interruption([], phases)


class IsolationTest(unittest.TestCase):
    """Protect input identities and pre-existing artifact directories."""

    def test_unignored_repository_output_is_rejected(self):
        """Relocating the tools must not bypass private-output protection at the root."""
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            output = repository / "unexpected-output"
            script = repository / "tools/drive-ref/drive_reference.py"
            with patch.object(reference, "__file__", str(script)), \
                    patch.object(reference.subprocess, "run") as ignored:
                ignored.return_value.returncode = 1
                with self.assertRaisesRegex(ValueError, "git-ignored"):
                    reference.check_output_directory(output)
            self.assertFalse(output.exists())

    def test_rom_identity_is_required(self):
        """A mismatched source must not create a staged ROM."""
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "input.bin"
            target = Path(temporary) / "output.bin"
            source.write_bytes(b"input")
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                reference.verified_copy(source, target, {"size": 5, "sha256": "wrong"})
            self.assertFalse(target.exists())

    def test_existing_directory_is_preserved(self):
        """A rerun cannot silently reuse or overwrite previous reference evidence."""
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            sentinel = output / "sentinel"
            sentinel.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                reference.check_output_directory(output)
            self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_capture_ram_is_reserved_before_open(self):
        """BASIC strings must not overwrite the mailbox or spuriously signal completion."""
        program = reference.basic_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        self.assertLess(program.index(":clr"), program.index("open15"))
        self.assertEqual(program, program.lower())


if __name__ == "__main__":
    unittest.main()
