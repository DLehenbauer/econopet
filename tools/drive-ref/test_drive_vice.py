# SPDX-License-Identifier: CC0-1.0
import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import drive_reference as reference
import drive_vice as vice


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
        expected = bytearray(174848)
        header = reference.image_offset("4040", 18, 0)
        expected[header:header + 2] = bytes((18, 1))
        expected[header + 144:header + 160] = b"REFERENCE".ljust(16, b"\xa0")
        expected[header + 162:header + 164] = b"AB"
        for track in range(1, 36):
            entry = header + track * 4
            free = range(2 if track == 18 else 0, reference.image_sectors("4040", track))
            for sector in free:
                expected[entry] += 1
                expected[entry + 1 + sector // 8] |= 1 << (sector % 8)
        lines = []
        for track in range(1, 36):
            entry = reference.fixture_bam_entry("4040", expected, track)
            marks = "".join("." if expected[entry + 1 + sector // 8] & (1 << (sector % 8)) else "*"
                            for sector in range(reference.image_sectors("4040", track)))
            lines.append(f"{track:3d}  {marks}")
        listing = '0 "reference       " ab 2a\nEmpty image\n664 blocks free.\n'
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
                        text = listing.replace("reference", "corrupted") if fault == "label" else listing
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
