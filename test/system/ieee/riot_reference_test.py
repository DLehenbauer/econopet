#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Verify that independent-reference corruption cannot produce a successful comparison."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from riot_reference import validate_observations


class ObservationTests(unittest.TestCase):
    """Exercise integrity checks independently of MAME or the production RIOT state."""

    def test_exact_transaction_order_and_completion(self):
        """Accept observed values only when the complete stimulus order is preserved."""
        validate_observations("W 0 128\nR 0 255\nWAIT\n",
                              "W 0 128\nR 0 128 255\nWAIT\nDONE\n")

    def test_corruption_is_rejected(self):
        """Reject omitted, reordered, duplicate, malformed and incomplete observations."""
        request = "W 0 128\nR 0 255\nWAIT\n"
        responses = (
            "", "DONE\n",
            "W 0 128\nR 0 128 255\nWAIT\n",
            "W 0 128\nR 0 128 255\nWAIT\nDONE\nDONE\n",
            "W 0 128\nR 0 256 255\nWAIT\nDONE\n",
            "W 0 128\nR 1 128 255\nWAIT\nDONE\n",
            "W 0 128\nR 0 128 0\nWAIT\nDONE\n",
            "W 0 129\nR 0 128 255\nWAIT\nDONE\n",
            "R 0 128 255\nW 0 128\nWAIT\nDONE\n",
            "W 0 128\nR 0 128 255\nR 0 128 255\nWAIT\nDONE\n",
        )
        for response in responses:
            with self.subTest(response=response), self.assertRaises(ValueError):
                validate_observations(request, response)

    def test_replay_rejects_bad_records_and_wrong_reference_values(self):
        """Require the executable itself to fail closed, including on an actual mismatch."""
        records = (
            "", "WAIT\nDONE\n", "DONE\n", "W 0\n", "W 32 0\nDONE\n",
            "W 0 256\nDONE\n", "R 0 0 256\nDONE\n",
            "W 1 255\nW 0 128\nR 0 128 255\n",
            "W 1 255\nW 0 128\nR 0 0 255\nDONE\n",
            "W 1 255\nW 0 128\nR 0 128 0\nDONE\n",
            "W 1 255\nW 0 128\nR 0 128 255 extra\nDONE\n",
            "W 1 255\nW 0 128\nR 0 128 255\nDONE\nWAIT\n",
        )
        with tempfile.TemporaryDirectory(prefix="econopet-riot-integrity-") as directory:
            path = Path(directory) / "observations.txt"
            for record in records:
                path.write_text(record, encoding="ascii")
                with self.subTest(record=record):
                    result = subprocess.run([REPLAY, str(path)], capture_output=True,
                                            text=True, timeout=5, check=False)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertTrue(result.stderr)
            path.write_text("W 1 255\nW 0 128\nR 0 128 255\nDONE\n", encoding="ascii")
            subprocess.run([REPLAY, str(path)], capture_output=True, timeout=5, check=True)


if __name__ == "__main__":
    REPLAY = sys.argv.pop(1)
    unittest.main()
