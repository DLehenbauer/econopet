# SPDX-License-Identifier: CC0-1.0
"""Test drive_rom_verify.py using synthetic source, listings, and ROM bytes.

Cover byte-preserving assembler syntax translation, source composition and
digests, source-line provenance, and rejection of unsupported directives.
Check emitted-byte accounting, independent gap-fill coverage validation,
checksum placeholder ownership, mismatch reporting, and ROM identity checks.
Invalid or missing observations must fail rather than imply a successful match.

Tests use constructed listings and byte arrays, not an actual assembler.
Temporary fixtures are removed after each test. Only Python 3 is required,
with no downloads or proprietary ROM inputs. This suite checks verification
logic, not correspondence of real ROMs or drive behavior.

Run this suite from the repository root:

    python3 tools/drive-roms/test_drive_rom_verify.py -v
"""

import hashlib
from pathlib import Path
import tempfile
import unittest

import drive_rom_verify as verify


class TranslationTest(unittest.TestCase):
    """Protect byte-preserving spelling changes used by the source rebuild."""

    def test_literal_spelling(self):
        """Preserve space, semicolon, apostrophe, strings, and trailing comments."""
        cases = {
            " CMP #' ": " CMP #32",
            " CMP #'; ; comment": " CMP #59",
            " CMP #''": " CMP #39",
            " LDA #'I-$40": " LDA #73-$40",
            "T .BYT 'A;B',0 ; comment": "T !byte 65,59,66,0",
            " LDA (CB)Y": " LDA (CB),Y",
            " ASL A": " ASL",
            "T .WORD OPEN,CLOSE": "T !word OPEN,CLOSE",
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertEqual(verify.translate_line(source), expected)

    def test_units_stop_at_end_and_ignore_listing_directives(self):
        """Ignore archived trailing fragments without dropping literal spaces."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "master").write_text(".LIB CODE\n", encoding="ascii")
            (root / "code").write_text(
                ".PAGE 'CODE'\n.SKIP2\n LDA #' \n.END\nINVALID\n", encoding="ascii"
            )
            units = verify.source_units(root)
            text, provenance = verify.translate(root, units)
            self.assertEqual(text, "!cpu 6502\n* = 0\n LDA #32\n")
            self.assertEqual(provenance[3], ("code", 3, " LDA #32"))
            before = verify.source_digest(root, units)
            (root / "code").write_text("changed\n", encoding="ascii")
            self.assertNotEqual(verify.source_digest(root, units), before)

    def test_unknown_directives_fail(self):
        """Unrecognized assembler behavior must not be silently omitted."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "code").write_text(".UNKNOWN 1\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "unsupported directive"):
                verify.translate(root, ["master", "code"])


class CoverageTest(unittest.TestCase):
    """Ensure missing source bytes and known placeholders cannot hide defects."""

    def test_truncated_listing_data_has_full_coverage(self):
        """Cover the entire byte/word directive rather than its printed prefix."""
        provenance = {
            3: ("code", 1, "!byte " + ",".join(["1"] * 11)),
            4: ("code", 2, "!word A,B,C,D,E"),
            5: ("code", 3, "LDA #1"),
        }
        report = (
            " 3 ff00 0101010101010101...!byte ...\n"
            " 4 ff0b 0000000000000000...!word ...\n"
            " 5 ff15 a901               LDA #1\n"
        )
        result = verify.emitted_addresses(report, provenance)
        self.assertEqual(set(result), set(range(0xFF00, 0xFF17)))

    def test_overlapping_bytes_fail(self):
        """Do not let later listing entries replace earlier source ownership."""
        with self.assertRaisesRegex(ValueError, "overlapping"):
            verify.emitted_addresses(
                " 3 fffe a901 LDA #1\n 4 ffff ea NOP\n",
                {3: ("code", 1, "LDA #1"), 4: ("code", 2, "NOP")},
            )

    def test_empty_listing_fails(self):
        """Missing listing coverage cannot produce a successful comparison."""
        with self.assertRaisesRegex(ValueError, "no recognized"):
            verify.emitted_addresses("unrecognized listing\n", {})

    def test_fill_check_detects_incomplete_coverage(self):
        """A partially recognized listing must not prove full source agreement."""
        first = b"\xfc\xff" + bytes([0, 1, 0, 2])
        alternate = b"\xfc\xff" + bytes([0, 1, 255, 2])
        emitted = {0xFFFC: ("code", 1), 0xFFFD: ("code", 2), 0xFFFF: ("code", 3)}
        verify.validate_coverage(first, alternate, emitted)
        del emitted[0xFFFD]
        with self.assertRaisesRegex(ValueError, "listing coverage"):
            verify.validate_coverage(first, alternate, emitted)

    def test_fill_check_rejects_changed_bounds_or_bytes(self):
        """Reject assembly instability instead of treating it as a reserved gap."""
        first = b"\xff\xff\x01"
        for alternate in [b"", b"\xfe\xff\x01", b"\xff\xff\x02"]:
            with self.subTest(alternate=alternate):
                with self.assertRaises(ValueError):
                    verify.validate_coverage(first, alternate, {0xFFFF: ("code", 1)})

    def test_only_declared_placeholders_are_excluded(self):
        """Report mismatches independently of uncovered or checksum bytes."""
        emitted = {0xFFFC: ("code", 1), 0xFFFD: ("code", 2), 0xFFFE: ("code", 3)}
        result = verify.compare(
            bytes([0, 2, 3]), 0xFFFC, bytes([9, 2, 4, 99]), 0xFFFC, emitted,
            {0xFFFC: ("code", 1)},
        )
        self.assertEqual(result["status"], "mismatch")
        self.assertEqual(result["compared_bytes"], 2)
        self.assertEqual(result["matching_bytes"], 1)
        self.assertEqual(result["differences"][0]["address"], "$FFFE")
        self.assertEqual(result["uncovered_ranges"], [["$FFFF", "$FFFF"]])
        self.assertEqual(result["checksum_placeholders"][0]["rom_byte"], 9)

    def test_placeholder_identity_is_required(self):
        """A nonzero or relocated checksum source byte must fail explicitly."""
        for source_byte, owner in [(1, ("code", 1)), (0, ("other", 1))]:
            with self.subTest(source_byte=source_byte, owner=owner):
                with self.assertRaisesRegex(ValueError, "placeholder changed"):
                    verify.compare(
                        bytes([source_byte]), 0xFFFF, bytes([1]), 0xFFFF,
                        {0xFFFF: owner}, {0xFFFF: ("code", 1)},
                    )

    def test_missing_placeholder_fails(self):
        """Source omissions cannot silently turn an expected checksum into a hole."""
        with self.assertRaisesRegex(ValueError, "not all expected"):
            verify.compare(
                bytes([1]), 0xFFFF, bytes([1]), 0xFFFF,
                {0xFFFF: ("code", 1)}, {0xFFFE: ("code", 2)},
            )

    def test_comparison_requires_observation(self):
        """Reject empty or out-of-bounds coverage instead of reporting success."""
        with self.assertRaisesRegex(ValueError, "no bytes"):
            verify.compare(b"\0", 0xFFFF, b"\0", 0xFFFF, {}, {})
        with self.assertRaisesRegex(ValueError, "outside rebuilt"):
            verify.compare(b"\0", 0xFFFF, b"\0\0", 0xFFFE, {0xFFFE: ("code", 1)}, {})


class IdentityTest(unittest.TestCase):
    """Check local ROM identities before assembly or comparison."""

    def test_corrupt_rom_is_rejected(self):
        """An expected size alone is not a sufficient reference-ROM identity."""
        data = bytes(4096)
        manifest = {
            "drive_profiles": [{"id": "fixture", "roms": ["test"]}],
            "drive_roms": [{
                "part": "test", "role": "main DOS", "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }],
        }
        family = {"profile": "fixture", "start": 0xF000}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "test.bin").write_bytes(data)
            rom, identities = verify.load_rom(manifest, root, family)
            self.assertEqual(rom, data)
            self.assertEqual(identities[0]["size"], len(data))
            (root / "test.bin").write_bytes(b"\1" + data[1:])
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                verify.load_rom(manifest, root, family)


if __name__ == "__main__":
    unittest.main()
