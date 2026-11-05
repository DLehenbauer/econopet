# SPDX-License-Identifier: CC0-1.0
"""Protect VICE corroboration boundaries, ROM ordering and reconstructed protocol."""

import hashlib
import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import drive_reference as reference
import drive_vice as vice


def observations(model, variant):
    """Encode status and data records independently of the replay generator."""
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    payloads = {
        "format_startup": b"73,CBM DOS V" + (b"2" if model == "4040" else b"2.7") + trailer,
        "format_bad_syntax": (b"30" if variant == "duplicate" else b"34") + b",SYNTAX ERROR" + trailer,
        "format_success": b"00, OK" + trailer,
        "format_listing": reference.format_listing(model, variant),
        "format_initialize": b"00, OK" + trailer,
        "format_reopened": reference.format_listing(model, variant),
    }
    if variant == "duplicate":
        payloads["format_copied_file"] = b"\0 \0\1\2\3\4\5"
    result = {}
    for name, payload in payloads.items():
        final = 64 if name not in ("format_listing", "format_reopened", "format_copied_file") or (
            variant != "duplicate") else 0
        result[name] = {"bytes": list(payload), "kernal_st": [0] * (len(payload) - 1) + [final]}
    return result


class ViceReferenceTest(unittest.TestCase):
    """Keep emulator shortcuts explicit while rejecting incomplete observations."""

    def test_basic_is_in_address_not_socket_order(self):
        """Do not repeat the prior C000/B000/D000 ROM composition bug."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name, value in (("basic-4-b000.901465-23.bin", b"B"),
                                ("basic-4-c000.901465-20.bin", b"C"),
                                ("basic-4-d000.901465-21.bin", b"D")):
                (path / reference.CONTROLLERS[name]).write_bytes(value * 4096)
            self.assertEqual(vice.compose_basic(path), b"B" * 4096 + b"C" * 4096 + b"D" * 4096)

    def test_duplicate_fixture_uses_literal_petscii_header(self):
        """Correct c1541's host-case conversion without changing historical non-DUPLICATE inputs."""
        for variant, label in (("duplicate", "m1 fixture,01"), ("full", "M1 FIXTURE,01")):
            args = argparse.Namespace(model="8050", variant=variant, c1541=Path("c1541"), petcat=Path("petcat"))
            with patch.object(reference, "run", side_effect=RuntimeError("stop at fixture formatting")) as run:
                with self.assertRaisesRegex(RuntimeError, "stop at fixture"):
                    reference.prepare_image(vice.fixture_arguments(args), Path("unused"))
                self.assertEqual(run.call_args.args[0][2], label)

    def test_quick_protocol_matches_actual_three_model_native_trace_hashes(self):
        """Ground OPEN/CLOSE/PRINT/GET reconstruction in previously captured complete wire stimuli."""
        digests = {
            "4040": "526dbf43d6c97f4b289a194891348ef151ecb29f91320328526ddd626730a4e8",
            "8050": "a6e3631a3a54d4cb1326d85fcdd95d049364126f91e505a4bd0903b579205d4d",
            "8250": "c1f1c0cb2db0f3fefa7d7ac60ecd013c52e6d8af92108d3665cb873544736fd5",
        }
        for model, digest in digests.items():
            actions = vice.protocol_actions(model, "quick", observations(model, "quick"))
            text = "ieee-replay-v1\n" + "".join(" ".join(map(str, row)) + "\n" for row in actions)
            self.assertEqual(hashlib.sha256(text.encode()).hexdigest(), digest)

    def test_every_format_observation_and_termination_is_retained(self):
        """Replay every VICE record, including reopen and DUPLICATE's source-file prefix."""
        for model in reference.MODELS:
            for variant in ("full", "duplicate"):
                samples = observations(model, variant)
                actions = vice.protocol_actions(model, variant, samples)
                for name, sample in samples.items():
                    reads = [row for row in actions if row[0] == name and row[1] == "R"]
                    self.assertEqual([row[2] for row in reads], sample["bytes"])
                    self.assertEqual([row[4] for row in reads], [int(s == 64) for s in sample["kernal_st"]])
                self.assertEqual(actions[-1], ("control", "A", 0, 0, 0))
                self.assertEqual(actions[-4][2:], (0xef, 1, 0))

    def test_omitted_wrong_byte_or_eoi_cannot_be_replayed(self):
        """Do not convert a missing VICE suffix or wrong status into generated passing expectations."""
        for variant in ("full", "duplicate"):
            for name in observations("8050", variant):
                for field in (None, "bytes", "kernal_st"):
                    samples = observations("8050", variant)
                    if field is None:
                        del samples[name]
                    else:
                        samples[name][field][-1] ^= 1
                    with self.subTest(variant=variant, name=name, field=field), self.assertRaises(ValueError):
                        vice.protocol_actions("8050", variant, samples)

    def test_monitor_uses_completion_and_error_fences(self):
        """No remote-prompt timeout or arbitrary elapsed delay counts as completed BASIC execution."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            program = path / "controller.prg"
            program.write_bytes(b"\1\4\0\0")
            vice.write_monitors(path, "8050", program)
            run = (path / "run.mon").read_text()
            self.assertIn("watch store $7000\ncondition 1 if A == $ff", run)
            self.assertIn("condition 2 if @ram:$7000 == $01", run)
            self.assertIn("> $002a 03 04", run)
            self.assertIn("8:$c000 8:$ffff", (path / "finish.mon").read_text())

    def test_bam_requires_every_track_and_sector(self):
        """c1541 must decode the entire model geometry, not only its first map part."""
        for model, tracks in vice.TRACKS.items():
            text = "\n".join(f"{track:3d}  " + "." * reference.image_sectors(model, track)
                             for track in range(1, tracks + 1))
            self.assertEqual(len(vice.bam_observation(text, model)), tracks)
            for malformed in (text + "\n" + text.splitlines()[0], text.rsplit("\n", 1)[0], text[:-1]):
                with self.assertRaises(ValueError):
                    vice.bam_observation(malformed, model)

    def test_c1541_inspection_rejects_bad_metadata_and_mutation(self):
        """Check independent decoding, count/bit consistency and read-only evidence retention."""
        expected = reference.format_expected_image("4040", "full", bytes(174848))
        lines = []
        for track in range(1, 36):
            entry = reference.fixture_bam_entry("4040", expected, track)
            marks = "".join("." if expected[entry + 1 + sector // 8] & (1 << (sector % 8)) else "*"
                            for sector in range(reference.image_sectors("4040", track)))
            lines.append(f"{track:3d}  {marks}")
        listing = '0 "m4 formatted    " ab 2a\nEmpty image\n664 blocks free.\n'
        bam = "\n".join(lines)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            image = path / "image.d64"
            for index, fault in enumerate(("none", "label", "free", "bit", "count", "mutation")):
                data = bytearray(expected)
                if fault == "count":
                    data[reference.fixture_bam_entry("4040", data, 1)] -= 1
                image.write_bytes(data)

                def run(command, log):
                    """Supply independently decoded output or one targeted inspection failure."""
                    if "-list" in command:
                        text = listing.replace("formatted", "corrupted") if fault == "label" else listing
                        text = text.replace("664", "663") if fault == "free" else text
                    else:
                        text = bam.replace(".", "*", 1) if fault == "bit" else bam
                        if fault == "mutation":
                            image.write_bytes(b"\xff" + image.read_bytes()[1:])
                    Path(log).write_text(text)

                with patch.object(reference, "run", side_effect=run):
                    target = path / str(index)
                    if fault == "none":
                        self.assertEqual(vice.inspect_image(Path("c1541"), "4040", image, target, data)["files"], {})
                    else:
                        with self.subTest(fault=fault), self.assertRaises(ValueError):
                            vice.inspect_image(Path("c1541"), "4040", image, target, data)


if __name__ == "__main__":
    unittest.main()
