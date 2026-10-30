#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Check original-controller evidence integrity without external ROMs or MAME."""

import unittest

from controller_reference import compare_observations, image, parse_observations, requests


class ControllerReferenceTests(unittest.TestCase):
    """Ensure agreement cannot result from truncated captures or overbroad SEEK normalization."""

    def setUp(self):
        """Create a small complete READ capture with distinct header and buffer sentinels."""
        self.header = bytes((73, 68, 18, 0, 0x55, 0xaa, 0x33, 0xcc))
        self.buffer = bytes(range(256)).hex()
        self.request = f"probe 0 128 {self.header.hex()} {self.buffer}\n"
        self.response = f"probe 0 1 {self.header.hex()} {self.buffer}\nDONE\n"

    def test_exact_capture_and_byte_mutation(self):
        """Accept exact observations and retain a single changed buffer byte as a mismatch."""
        self.assertEqual(compare_observations(self.request, self.response, self.response), [])
        changed = self.response.replace(self.buffer, "ff" + self.buffer[2:])
        self.assertEqual(len(compare_observations(self.request, self.response, changed)), 1)

    def test_bad_capture_rejected(self):
        """Reject omitted, duplicated, misidentified, uncompleted and malformed observations."""
        for response in ("", "DONE\n", self.response.removesuffix("DONE\n"),
                         self.response + "DONE\n", self.response.replace("probe", "wrong"),
                         self.response.replace("probe 0", "probe 1"),
                         self.response.replace(" 1 ", " 128 "),
                         self.response.replace(self.buffer, "xx" + self.buffer[2:]),
                         self.response.replace(self.buffer, self.buffer[:-2])):
            with self.subTest(response=response), self.assertRaises(ValueError):
                parse_observations(self.request, response)

    def test_only_valid_rotational_seek_fields_are_normalized(self):
        """Allow differing valid sector/checksum pairs, never header corruption or stale bytes."""
        request = self.request.replace(" 128 ", " 176 ")
        headers = []
        for sector in (1, 13):
            header = bytearray(self.header)
            header[3] = sector
            header[4] = header[0] ^ header[1] ^ header[2] ^ sector
            headers.append(header)
        before, after = (self.response.replace(self.header.hex(), header.hex()) for header in headers)
        self.assertEqual(compare_observations(request, before, after), [])
        for index, value in ((0, ord("X")), (2, 19), (3, 19), (4, 0)):
            header = bytearray(headers[1])
            header[index] = value
            corrupted = after.replace(headers[1].hex(), header.hex())
            with self.subTest(index=index), self.assertRaises(ValueError):
                compare_observations(request, before, corrupted)
        header = bytearray(headers[1])
        header[7] ^= 1
        changed = after.replace(headers[1].hex(), header.hex())
        self.assertEqual(len(compare_observations(request, before, changed)), 1)

    def test_requests_and_media_cover_both_drives_and_all_slots(self):
        """Keep routing observable with distinct images despite the reference's shared-ID bug."""
        rows = [record.split() for record in requests().splitlines()]
        self.assertEqual({int(row[1]) for row in rows}, set(range(15)))
        self.assertEqual({int(row[2]) & 1 for row in rows}, {0, 1})
        self.assertNotEqual(image(0), image(1))
        self.assertEqual(len(image(0)), 174848)
        for scenario in ("normal", "protected", "absent", "errors", "format"):
            self.assertTrue(requests(scenario))


if __name__ == "__main__":
    unittest.main()
