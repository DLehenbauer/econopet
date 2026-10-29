# SPDX-License-Identifier: CC0-1.0
"""Test drive_rom_map.py using synthetic source, manifests, and ROM bytes.

Cover source composition and overlays, source-line provenance, input identity
checks, checksum exclusions, emitted-byte coverage, and uncovered ranges.
Also exercise the source-mapping CLI, preservation of existing outputs, and
exact disassembly/reassembly round-trips, including rejection of corrupt output.

Fixtures are created in temporary directories and removed after each test.
No downloads or proprietary ROM inputs are required. Tests invoking ACME or
the CC65 tools (da65, ca65, ld65) are skipped when those tools are unavailable.
These are tooling regression tests, not drive behavior or hardware acceptance.

Run this suite from the repository root:

    python3 tools/drive-roms/test_drive_rom_map.py -v
"""

import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import drive_rom_map as mapping


class TranslationTest(unittest.TestCase):
    """Keep declared semantic edits separate from spelling-only translation."""

    def test_composition_and_overlay(self):
        """Retain source provenance and Unicode trailers without executing them."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "lib").write_text(".LIB CODE\n.END\n.LIB IGNORE\n", encoding="ascii")
            (root / "code").write_text(
                "*=$FFFC\nSTART LDA #1\n RTS\n.END\ntrailer \u201d\n", encoding="utf-8"
            )
            units = mapping.composition(root, "lib")
            self.assertEqual(units, ["lib", "code"])
            text, provenance = mapping.translate_mapping(root, units, [
                {"unit": "code", "first": 2, "last": 3,
                 "replacement": ["START LDX #1", "INX", "RTS"]}
            ])
            self.assertEqual(
                text, "!cpu 6502\n* = 0\n*=$FFFC\nSTART LDX #1\nINX\nRTS\n"
            )
            self.assertEqual(provenance[5], ("code", 2, "INX"))

    def test_invalid_patch_ranges(self):
        """Reject missing, overlapping, reversed, and post-END edits."""
        invalid = [
            [{"unit": "other", "first": 1, "last": 1, "replacement": []}],
            [{"unit": "code", "first": 0, "last": 1, "replacement": []}],
            [{"unit": "code", "first": 2, "last": 1, "replacement": []}],
            [{"unit": "code", "first": 3, "last": 4, "replacement": []}],
            [{"unit": "code", "first": "1", "last": 2, "replacement": []}],
            [{"unit": "code", "first": 1, "last": 2, "replacement": "RTS"}],
            [{"unit": "code", "first": 1, "last": 2, "replacement": []},
             {"unit": "code", "first": 2, "last": 2, "replacement": []}],
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "code").write_text("LDA #1\nRTS\n.END\n", encoding="ascii")
            for patches in invalid:
                with self.subTest(patches=patches), self.assertRaises(ValueError):
                    mapping.translate_mapping(root, ["lib", "code"], patches)

    def test_unsupported_source_fails(self):
        """Never silently discard unsupported directives or non-ASCII code."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for text in [".UNKNOWN 1\n", "LDA #'\u00e9\n"]:
                (root / "code").write_text(text, encoding="utf-8")
                with self.subTest(text=text), self.assertRaises(ValueError):
                    mapping.translate_mapping(root, ["lib", "code"], [])

    def test_invalid_compositions(self):
        """Reject path escapes and repeated units rather than changing input scope."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                mapping.composition(root, "../master")
            for text in [".END\n", ".LIB CODE\n.LIB CODE\n"]:
                (root / "lib").write_text(text, encoding="ascii")
                with self.subTest(text=text), self.assertRaises(ValueError):
                    mapping.composition(root, "lib")


class ComparisonTest(unittest.TestCase):
    """Require complete emitted coverage and explicit checksum provenance."""

    def test_mismatches_and_holes_remain_visible(self):
        """Report data changes separately from checksum exclusions and padding."""
        result = mapping.compare_mapping(
            0xFFFC, bytes([0, 2, 3, 0]),
            {0xFFFC: ("code", 1), 0xFFFD: ("code", 2), 0xFFFE: ("code", 3)},
            bytes([9, 2, 4, 170]),
            {"start": 0xFFFC, "checksums": [
                {"address": 0xFFFC, "unit": "code", "line": 1, "source_value": 0}
            ]},
        )
        self.assertEqual(result["compared_bytes"], 2)
        self.assertEqual(result["matching_bytes"], 1)
        self.assertEqual(result["mismatch_count"], 1)
        self.assertEqual(result["differences"][0]["address"], 0xFFFE)
        self.assertEqual(result["uncovered_ranges"], [["$FFFF", "$FFFF"]])
        self.assertEqual(result["uncovered_values"], [170])

    def test_checksum_identity_is_not_a_generic_mask(self):
        """Reject changed source values, ownership, missing coverage, or duplicates."""
        base = {"address": 0xFFFF, "unit": "code", "line": 1, "source_value": 37}
        cases = [
            [dict(base, source_value=0)], [dict(base, unit="other")],
            [dict(base, line=2)], [dict(base, address=0xFFFE)], [base, base],
        ]
        for exclusions in cases:
            with self.subTest(exclusions=exclusions), self.assertRaises(ValueError):
                mapping.compare_mapping(
                    0xFFFF, b"\x25", {0xFFFF: ("code", 1)}, b"\xa3",
                    {"start": 0xFFFF, "checksums": exclusions},
                )

    def test_source_map_keeps_relocation_and_deletion(self):
        """Avoid presenting removed instructions as unchanged mapped routines."""
        result = mapping.source_map(
            {0xFFFC: ("code", 1), 0xFFFD: ("code", 2)},
            {0xFFFE: ("code", 1), 0xFFFF: ("code", 1)},
        )
        self.assertEqual(result, [
            {"unit": "code", "line": 1,
             "original": [["$FFFC", "$FFFC"]], "mapped": [["$FFFE", "$FFFF"]]},
            {"unit": "code", "line": 2,
             "original": [["$FFFD", "$FFFD"]], "mapped": []},
        ])

    def test_approved_identity_and_membership(self):
        """Reject altered bytes, unapproved parts, and repeated chip composition."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "chip.bin").write_bytes(b"\x01")
            manifest = {
                "drive_profiles": [{"id": "profile", "roms": ["chip"]}],
                "drive_roms": [{"part": "chip", "size": 1,
                               "sha256": hashlib.sha256(b"\x01").hexdigest()}],
            }
            recipe = {"profile": "profile", "parts": ["chip"], "start": 0xFFFF}
            self.assertEqual(mapping.approved_rom(manifest, root, recipe), (b"\x01", ["chip"]))
            for parts in [[], ["other"], ["chip", "chip"]]:
                with self.subTest(parts=parts), self.assertRaises(ValueError):
                    mapping.approved_rom(manifest, root, dict(recipe, parts=parts))
            (root / "chip.bin").write_bytes(b"\x02")
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                mapping.approved_rom(manifest, root, recipe)

    @unittest.skipUnless(shutil.which("acme"), "ACME is required for assembly")
    def test_real_assembly_coverage_and_output_retention(self):
        """Check actual emitted zero bytes independently of gap-fill defaults."""
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "assembly"
            origin, data, emitted = mapping.assemble(
                output, "!cpu 6502\n*=$FFFA\nLDA #0\n*=$FFFF\nRTS\n",
                {3: ("code", 1, "LDA #0"), 5: ("code", 2, "RTS")},
            )
            self.assertEqual(origin, 0xFFFA)
            self.assertEqual(data, b"\xa9\0\0\0\0\x60")
            self.assertEqual(set(emitted), {0xFFFA, 0xFFFB, 0xFFFF})
            before = (output / "source.prg").read_bytes()
            with self.assertRaises(FileExistsError):
                mapping.assemble(output, "invalid", {})
            self.assertEqual((output / "source.prg").read_bytes(), before)

    @unittest.skipUnless(shutil.which("acme"), "ACME is required for assembly")
    def test_source_mapping_cli_and_identity_failures(self):
        """Exercise manifest-driven mapping and identity failures before output."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "master").write_text(".LIB CODE\n.END\n", encoding="ascii")
            original = "*=$FFFC\nLDA #1\nRTS\n.BYTE 0\n.END\n"
            (root / "code").write_text(original, encoding="ascii")
            rom = b"\xa9\x02\x60\x00"
            (root / "chip.bin").write_bytes(rom)
            manifest = {
                "source": {"commit": mapping.verify.SOURCE_COMMIT},
                "source_mappings": {"fixture": {
                    "profile": "profile", "master": "master", "start": 0xFFFC,
                    "parts": ["chip"], "checksums": [],
                    "source_digest": mapping.verify.source_digest(root, ["master", "code"]),
                    "patches": [{"unit": "code", "first": 2, "last": 2,
                                 "replacement": ["LDA #2"]}],
                }},
                "drive_profiles": [{"id": "profile", "roms": ["chip"]}],
                "drive_roms": [{"part": "chip", "size": len(rom),
                               "sha256": hashlib.sha256(rom).hexdigest()}],
            }
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="ascii")
            output = root / "output"
            args = [
                "drive_rom_map.py", "--mapping", "fixture",
                "--source-dir", str(root), "--rom-dir", str(root),
                "--manifest", str(manifest_path), "--output-dir", str(output),
            ]
            with patch("sys.argv", args), patch("builtins.print"):
                self.assertEqual(mapping.main(), 0)
            report = json.loads((output / "mapping.json").read_text())
            self.assertEqual(report["matching_bytes"], len(rom))
            with patch("sys.argv", args), self.assertRaises(FileExistsError):
                mapping.main()
            args[-1] = str(root / "invalid")
            (root / "code").write_text(original + "\n", encoding="ascii")
            with patch("sys.argv", args), self.assertRaisesRegex(ValueError, "pinned source"):
                mapping.main()
            (root / "code").write_text(original, encoding="ascii")
            (root / "chip.bin").write_bytes(b"\0" * len(rom))
            with patch("sys.argv", args), self.assertRaisesRegex(ValueError, "ROM identity"):
                mapping.main()
            self.assertFalse((root / "invalid").exists())


class DisassemblyTest(unittest.TestCase):
    """Reconstruct synthetic payloads without treating table bytes as code."""

    def setUp(self):
        """Create an original short program with a trailing pointer table."""
        self.data = bytes([0xA9, 1, 0x60, 0x00, 0x05])
        self.recipe = {
            "offset": 0, "size": len(self.data), "runtime": 0x0500,
            "payload_sha256": hashlib.sha256(self.data).hexdigest(),
            "data_ranges": [[0x0503, 0x0504, "word"]],
            "entries": {"ENTRY": 0x0500},
        }

    def test_invalid_identity_and_classification(self):
        """Reject invalid bounds, overlapping tables, and entries inside data."""
        cases = [
            dict(self.recipe, offset=-1), dict(self.recipe, size=6),
            dict(self.recipe, payload_sha256="0" * 64),
            dict(self.recipe, data_ranges=[[0x0503, 0x0503, "word"]]),
            dict(self.recipe, data_ranges=[[0x0503, 0x0504, "code"]]),
            dict(self.recipe, data_ranges=[[0x0503, 0x0504, "byte"]] * 2),
            dict(self.recipe, entries={"ENTRY": 0x0503}),
            dict(self.recipe, entries={"BAD;NAME": 0x0500}),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output"
            for recipe in cases:
                with self.subTest(recipe=recipe), self.assertRaises(ValueError):
                    mapping.disassemble(output, self.data, recipe)
                self.assertFalse(output.exists())

    @unittest.skipUnless(
        all(shutil.which(tool) for tool in ("da65", "ca65", "ld65")),
        "CC65 tools are required for disassembly reconstruction",
    )
    def test_actual_reconstruction_and_mismatch(self):
        """Keep complete byte equality observable and reject a changed rebuild."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = mapping.disassemble(root / "valid", self.data, self.recipe)
            self.assertEqual(result["payload_bytes"], len(self.data))
            self.assertIn("WordTable", (root / "valid/source.info").read_text())
            self.assertEqual((root / "valid/rebuilt.bin").read_bytes(), self.data)
            with self.assertRaises(FileExistsError):
                mapping.disassemble(root / "valid", self.data, self.recipe)
            original_run = mapping.subprocess.run

            def corrupt_link(command, **kwargs):
                """Mutate only the synthetic linked output to exercise rejection."""
                result = original_run(command, **kwargs)
                if command[0] == "ld65":
                    Path(command[command.index("-o") + 1]).write_bytes(b"\x60")
                return result

            with patch.object(mapping.subprocess, "run", side_effect=corrupt_link):
                with self.assertRaisesRegex(ValueError, "reconstruction differs"):
                    mapping.disassemble(root / "corrupt", self.data, self.recipe)


if __name__ == "__main__":
    unittest.main()
