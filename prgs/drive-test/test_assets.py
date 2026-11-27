#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Test deterministic Drive Test image generation and base-RAM PRG bounds."""

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def load_module(name: str, path: Path):
    """Load a local Python script from path under module name."""
    # Construct and execute a module spec directly so hyphenated parent directories need no package init.
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


prg_check = load_module("drive_test_prg_check", ROOT / "check_prg.py")
vice_vet = load_module("drive_test_vet_vice", ROOT / "vet_vice.py")


class DriveTestAssetTests(unittest.TestCase):
    """Check base-RAM PRG bounds and VICE $7FF0 mailbox decoding."""

    def test_prg_must_fit_entirely_below_expansion_window(self):
        """Verify that check_prg accepts valid base-RAM layouts and rejects load, BSS, or SYS faults."""
        with tempfile.TemporaryDirectory() as temporary:
            # Build a minimal valid PRG and linker map at $0401 with a matching SYS 1037 stub.
            prg = Path(temporary) / "test.prg"
            map_file = Path(temporary) / "test.map"
            map_file.write_text(
                "Segment list:\n"
                "EXEHDR 000401 00040C 00000C 00001\n"
                "STARTUP 00040D 00040D 000001 00001\n"
                "CODE 00040E 00040E 000001 00001\n"
                "BSS 00040F 00040F 000001 00001\n"
                "Exports list by name:\n"
            )
            launcher = b"\x0b\x04\x20\x03\x9e1037\x00\x00\x00"
            prg.write_bytes(b"\x01\x04" + launcher + b"\x60\x60")
            self.assertEqual(prg_check.main(prg, map_file), 0)

            # Reject a load address outside $0401 or intruding into reserved stack/mailbox space.
            prg.write_bytes(b"\xef\x7f\x60")
            with self.assertRaisesRegex(ValueError, "does not fit below"):
                prg_check.main(prg, map_file)

            # Reject a BSS segment colliding with the default $0800 stack ($77F0..$7FEF) below $7FF0.
            prg.write_bytes(b"\x01\x04" + launcher + b"\x60\x60")
            map_file.write_text(map_file.read_text().replace(
                "BSS 00040F 00040F 000001", "BSS 0077EF 0077F0 000002"))
            with self.assertRaisesRegex(ValueError, "invalid resident segment BSS"):
                prg_check.main(prg, map_file)

            # Reject a BASIC SYS target that disagrees with the STARTUP segment base.
            map_file.write_text(map_file.read_text().replace(
                "BSS 0077EF 0077F0 000002", "BSS 00040F 00040F 000001"))
            prg.write_bytes(b"\x01\x04" + launcher.replace(b"1037", b"1038") + b"\x60\x60")
            with self.assertRaisesRegex(ValueError, "invalid BASIC SYS launcher"):
                prg_check.main(prg, map_file)

            # Honor a custom __STACKSIZE__ export ($0200 -> stack at $7DF0..$7FEF) when checking BSS bounds.
            map_file.write_text(
                "Segment list:\n"
                "EXEHDR 000401 00040C 00000C 00001\n"
                "STARTUP 00040D 00040D 000001 00001\n"
                "CODE 00040E 00040E 000001 00001\n"
                "BSS 007DEF 007DF0 000002 00001\n"
                "Exports list by name:\n"
                "__STACKSIZE__ 000200 REA\n"
            )
            prg.write_bytes(b"\x01\x04" + launcher + b"\x60\x60")
            with self.assertRaisesRegex(ValueError, "invalid resident segment BSS"):
                prg_check.main(prg, map_file)

    def test_vice_helpers_decode_result_and_symbols(self):
        """Verify CC65 .lbl symbol lookup and VICE bsave mailbox decoding."""
        # Resolve an underscore-prefixed exported symbol from CC65's label format.
        labels = "al 003E18 ._drive_fail_halt\n"
        self.assertEqual(vice_vet.resolve_symbol(labels, "drive_fail_halt"), 0x3E18)

        # Decode a 2-byte-prefixed VICE bsave dump of the $7FF0 diagnostic mailbox.
        decoded = vice_vet.decode_result(
            b"\xf0\x7fTD\x00\x02\xd4\x01\x03\x31\x30\x00\x00\x0d\x00\x00\x00\x00"
        )
        self.assertEqual(decoded["magic0"], ord("T"))
        self.assertEqual(decoded["magic1"], ord("D"))
        self.assertEqual(decoded["state"], 2)
        self.assertEqual(decoded["completed"], 468)
        self.assertEqual(decoded["detail_id"], 3)
        self.assertEqual(decoded["offset"], 13)


if __name__ == "__main__":
    unittest.main()
