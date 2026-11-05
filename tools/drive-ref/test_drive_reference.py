# SPDX-License-Identifier: CC0-1.0
"""Protect reference isolation, bounded captures and native regression observations."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import drive_reference as reference
import build_drive_reference as builder


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


def native_command_observations(model="4040"):
    """Construct independent status, retained-prefix and closed-source expectations."""
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    startup = b"73,CBM DOS V" + (b"2" if model == "4040" else b"2.7") + trailer
    payloads = {
        "command_startup": startup, "command_prefix": b"\0 ",
        "command_u0": b"00, OK" + trailer, "command_short_u": b"00, OK" + trailer,
        "command_u0_cursor": b"\0\1", "command_initialize": b"00, OK" + trailer,
        "command_closed": b"\r", "command_reopen": b"\0 ",
        "command_reset": startup, "command_reset_closed": b"\r",
        "command_reset_reopen": b"\0 ", "command_unknown": b"31,SYNTAX ERROR" + trailer,
    }
    return {
        name: {"bytes": list(payload), "kernal_st":
               [2] if name in ("command_closed", "command_reset_closed")
               else [0] * len(payload) if name in (
                   "command_prefix", "command_u0_cursor", "command_reopen", "command_reset_reopen")
               else [0] * (len(payload) - 1) + [64]}
        for name, payload in payloads.items()
    }


def native_edge_observations(model, variant):
    """Encode the edge matrix independently of the production observation checker."""
    expected = {
        "structure": [73, 34, 34] + [30] * 11 + [34, 30, 30, 34, 30, 30, 34, 34] + [31] * 4,
        "precedence": [73, b"\0 ", b"\x0b"] + [70] * 12
                      + [(66, 0, 255), (66, 255, 255), 30, 31, 30, 62, 30, 62, 62, 30, 0, b"\0\1\2\3"],
        "numbers": [73, 0, b"\x0b"] + [0] * 12 + [b"\xc8"],
        "numeric-errors": [73, 0, b"\x0b"] + [30] * 6
                   + [(66, 0, 0), (66, 0, 0), (66, 255, 255), (66, 0, 0), (66, 0, 0), (66, 0, 0),
                      (66, 1, 255)],
        "direct": [73, 0, 0, 0, 0, 0, 0, 70, 0, 70, 0, 0, 0, 0, 0, 70, 0, 0, 0, 0,
                   0, 0, 0, 30, 0, 0, 0, 70],
        "direct-values": [73, 0, 0, b"\0", 0, b"\0", 0, b"\x0a", 0, b"\x02", 0, b"\x03",
                          70, 0, b"\x03", 0, b"\x0b"],
        "cache-drives": [73, 0, (0, 0, 0, 1), b"\x0b", 0, b"\0 ", (73, 1, 1, 1),
                         (0, 0, 0, 1), None, b"\x04", 0, 0, 0],
        "cache-parts": [73, 0, b"\x0b", 0, (73, 1, 1) if model == "4040" else 0,
                        0, (73, 1, 2), 0, (73, 1, 3) if model == "4040" else 0,
                        0, b"\x0b", 0, bytes(4)],
        "cache-raw": [73, 0, b"\x0b", 0, 0, 0, 0, 0, b"\x0b", 0, (73, 1, 2), 0, bytes(4)],
        "cache-errors": [73, 0, b"\x0b", (66, 0, 0), 70, (66, 0, 0), (66, 1, 255), (73, 1, 1),
                         62, 63, 62, 63, (1, 0, 0), 30, 30, b"\0 \0\1"],
        "limits": [73, 31, 31, 32, 32, 31, 32, 31, 31, 0, b"\0 \0\1"],
        "cache-metadata": [73, 0, (73, 18 if model == "4040" else 39, 1), 0,
                           (73, 18 if model == "4040" else 39, 1), 0,
                           (73, 18 if model == "4040" else 39, 1), 0,
                           (73, 18 if model == "4040" else 38, 0), 0,
                           (73, 18 if model == "4040" else 38, {"4040": 0, "8050": 3, "8250": 9}[model]), 0,
                           (73, 18 if model == "4040" else 38, {"4040": 0, "8050": 3, "8250": 9}[model]), 0,
                           b"\0 \0\1"],
        "cache-dirty": [73, 0, b"\x0b", 0, 0, b"\xff", 0, 0, b"\xfb", b"\x0b", 0, b"\xfd"],
        "validate-cross": [73, 0, 0 if model == "4040" else (65, 38, 1),
                           0 if model == "4040" else (65, 38, 1), b"\0 \0\1"],
        "validate-side": [73, 0, 0, 0, b"\0 \0\1"],
    }[variant]
    names = [step[0] for step in reference.edge_steps(model, variant)]
    if len(expected) != len(names):
        raise ValueError(f"independent edge oracle case count: {variant}")
    messages = {0: b" OK", 1: b" FILES SCRATCHED", 30: b"SYNTAX ERROR",
                31: b"SYNTAX ERROR", 32: b"SYNTAX ERROR", 34: b"SYNTAX ERROR", 62: b" FILE NOT FOUND",
                63: b" FILE EXISTS", 65: b"NO BLOCK", 66: b"ILLEGAL TRACK OR SECTOR", 70: b"NO CHANNEL",
                73: b"CBM DOS V2" + (b"" if model == "4040" else b".7")}
    observations = {}
    for name, value in zip(names, expected):
        if value is None:
            payload, states = b"\r", [2]
        elif isinstance(value, bytes):
            payload, states = value, [0] * len(value)
        else:
            code, track, sector = value[:3] if isinstance(value, tuple) else (value, 0, 0)
            drive = value[3] if isinstance(value, tuple) and len(value) == 4 else 0
            if model == "4040":
                track, sector = track % 100, sector % 100
            payload = (f"{code:02d},".encode() + messages[code]
                       + f",{track:02d},{sector:02d}".encode()
                       + (b"\r" if model == "4040" else f",{drive}\r".encode()))
            states = [0] * (len(payload) - 1) + [64]
        observations[name] = {"bytes": list(payload), "kernal_st": states}
    return observations


class EdgeObservationTest(unittest.TestCase):
    """Require every edge case, exact state flags and complete persisted images."""

    def test_parser_stimuli_keep_initialization_fences_and_literal_alias(self):
        """Do not confuse media initialization, asynchronous CLOSE or PETCAT case folding with parsing."""
        for variant in ("numbers", "numeric-errors"):
            steps = reference.edge_steps("4040", variant)
            self.assertEqual(steps[1][1], 'PRINT#15,"I0"')
            self.assertEqual(steps[2][1], 'OPEN2,8,2,"#"')
        self.assertIn('chr$(97)', reference.edge_program("4040", "numbers"))
        for variant in ("direct", "direct-values"):
            steps = reference.edge_steps("4040", variant)
            for index, step in enumerate(steps):
                if step[0].startswith("direct_name_") or (
                        step[1].startswith("OPEN2,") and step[0] != "direct_number"):
                    self.assertTrue(steps[index - 1][0].startswith("direct_close_"))
                    self.assertEqual(steps[index - 1][2], 15)
                    self.assertNotIn("CLOSE", step[1])

    def test_independent_observations_and_roundtrips(self):
        """Check all short records against a separate oracle and bounded RAM layout."""
        for model in reference.MODELS:
            for variant in reference.EDGE_VARIANTS:
                with self.subTest(model=model, variant=variant):
                    observed = native_edge_observations(model, variant)
                    layout = reference.edge_layout(model, variant)
                    steps = reference.edge_steps(model, variant)
                    reference.check_block_observations(model, observed, steps, layout, "edge")
                    capture, mailbox = bytearray(0x1c00), bytearray(32)
                    mailbox[0] = reference.COMPLETE
                    occupied = set()
                    for name, (data, status, count, limit) in layout.items():
                        self.assertLess(count, 31)
                        sample = observed[name]
                        self.assertLessEqual(len(sample["bytes"]), limit)
                        mailbox[count] = len(sample["bytes"])
                        for address, field in ((data, "bytes"), (status, "kernal_st")):
                            start = address - reference.CAPTURE_ADDRESS
                            region = set(range(start, start + limit))
                            self.assertFalse(occupied & region)
                            occupied |= region
                            self.assertLessEqual(start + limit, len(capture))
                            capture[start:start + len(sample[field])] = bytes(sample[field])
                    self.assertEqual(reference.decode_capture(capture, mailbox, layout), observed)

    def test_missing_extra_bytes_and_eoi_are_not_accepted(self):
        """Reject omitted cases and a wrong byte or KERNAL flag in every observation."""
        for variant in reference.EDGE_VARIANTS:
            steps = reference.edge_steps("8050", variant)
            layout = reference.edge_layout("8050", variant)
            for name in layout:
                for field in ("bytes", "kernal_st", None):
                    observed = native_edge_observations("8050", variant)
                    if field is None:
                        del observed[name]
                    else:
                        observed[name][field][-1] ^= 1
                    with self.subTest(variant=variant, name=name, field=field), self.assertRaises(ValueError):
                        reference.check_block_observations("8050", observed, steps, layout, "edge")
            observed = native_edge_observations("8050", variant)
            observed["unexpected"] = observed["edge_startup"]
            with self.assertRaisesRegex(ValueError, "coverage"):
                reference.check_block_observations("8050", observed, steps, layout, "edge")

    def test_program_keeps_binary_cr_and_all_phase_markers(self):
        """Do not truncate raw data at CR or merge the protected per-case buffers."""
        for variant in reference.EDGE_VARIANTS:
            program = reference.edge_program("4040", variant)
            self.assertIn("if(f<>15orv<>13)andz=0", program)
            for index in range(len(reference.edge_steps("4040", variant))):
                self.assertIn(f"poke{reference.PHASE_ADDRESS},{reference.EDGE_PHASE_START + index}:", program)
            self.assertIn(f"poke{reference.BASIC_MEMSIZ_HIGH},64", program)

    def test_exact_images_and_mutation_negatives(self):
        """Cache acceptance may change only the explicitly allowed version and raw pages."""
        with tempfile.TemporaryDirectory() as directory:
            initial, final = Path(directory) / "initial", Path(directory) / "final"
            for model, size in (("4040", 174848), ("8050", 533248), ("8250", 1066496)):
                raw = bytearray((index * 17 + 3) % 256 for index in range(size))
                initial.write_bytes(raw)
                for variant in reference.EDGE_VARIANTS:
                    original = reference.format_expected_image(model, "quick", bytes(size)) if (
                        variant.startswith("validate-")) else raw
                    initial.write_bytes(original)
                    expected = bytearray(original)
                    if variant == "cache-parts" and model != "4040":
                        expected[256:512] = raw[:256]
                        expected[768:1024] = raw[:256]
                    if variant == "cache-raw":
                        offset = reference.image_offset(
                            model, 18 if model == "4040" else 38, {"4040": 0, "8050": 3, "8250": 9}[model])
                        expected[offset + 2] = 255
                        expected[256:512] = raw[:256]
                    if variant in ("cache-dirty", "cache-drives"):
                        # The synthetic map only needs one exact count/bitmap claim.
                        entry = reference.image_offset(model, 18 if model == "4040" else 38, 0)
                        if model != "4040":
                            original[entry + 4:entry + 6] = bytes((1, 51))
                            initial.write_bytes(original)
                            expected = bytearray(original)
                        entry += 4 if model == "4040" else 6
                        expected[entry] -= 1
                        expected[entry + 1] &= ~(2 if variant == "cache-dirty" else 4)
                        if variant == "cache-drives":
                            expected[256:512] = original[:256]
                    final.write_bytes(expected)
                    reference.check_edge_media(model, variant, initial, final)
                    for offset in (0, 255, 256, 511, 512, 768, len(raw) - 1):
                        changed = bytearray(expected)
                        changed[offset] ^= 1
                        final.write_bytes(changed)
                        with self.subTest(model=model, variant=variant, offset=offset), self.assertRaisesRegex(
                                ValueError, "complete-image"):
                            reference.check_edge_media(model, variant, initial, final)

    def test_fixture_versions_are_per_map_part_not_header_only(self):
        """A mixed-version input must leave later maps native and all probes unallocated."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture"
            for model, size in (("4040", 174848), ("8050", 533248), ("8250", 1066496)):
                for variant in ("cache-parts", "cache-errors", "cache-raw"):
                    raw = reference.format_expected_image(model, "quick", bytes(size))
                    path.write_bytes(raw)
                    reference.edge_fixture(model, variant, path)
                    data = bytearray(path.read_bytes())
                    self.assertEqual(data[:256], reference.BLOCK_READ_PAGE)
                    self.assertEqual(data[256:1024], bytes(768))
                    entry = reference.fixture_bam_entry(model, data, 1)
                    self.assertEqual(data[entry + 1] & 15, 15)
                    parts = 1 if model == "4040" else 2 if model == "8050" else 4
                    for part in range(parts):
                        offset = reference.image_offset(model, 18 if model == "4040" else 38, part * 3)
                        expected = 255 if variant == "cache-errors" or (
                            variant == "cache-parts" and part == 0) else ord("A" if model == "4040" else "C")
                        self.assertEqual(data[offset + 2], expected)
                    data[entry + 1] &= ~4
                    path.write_bytes(data)
                    with self.assertRaisesRegex(ValueError, "four free"):
                        reference.edge_fixture(model, variant, path)


class NativeCommandTest(unittest.TestCase):
    """Keep the command oracle's exact channel/reset distinctions reproducible."""

    def test_all_native_models(self):
        """Preserve model-specific status while checking every command case."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_command_observations(model, native_command_observations(model))

    def test_incomplete_observations_fail(self):
        """An empty or partial collection is not command parity evidence."""
        for observations in ({}, {"command_reset": native_command_observations()["command_reset"]}):
            with self.subTest(observations=observations), self.assertRaisesRegex(ValueError, "coverage"):
                reference.check_command_observations("4040", observations)

    def test_u0_cannot_rewind_cursor_or_clear_stale_selector(self):
        """A reset-like U0 or cleared CMDBUF+1 must fail the independent oracle."""
        for name, payload in (("command_u0_cursor", b"\0 "),
                              ("command_short_u", b"30,SYNTAX ERROR,00,00\r")):
            observations = native_command_observations()
            observations[name]["bytes"] = list(payload)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_command_observations("4040", observations)

    def test_closed_source_requires_real_input_timeout(self):
        """Receiving a drive-supplied CR is not equivalent to a closed-channel timeout."""
        observations = native_command_observations()
        observations["command_closed"]["kernal_st"] = [64]
        with self.assertRaisesRegex(ValueError, "command_closed"):
            reference.check_command_observations("4040", observations)

    def test_command_capture_roundtrips(self):
        """Keep all command buffers distinct and decode their bounded mailbox counts."""
        capture, mailbox = bytearray(0x1c00), bytearray(32)
        mailbox[0] = reference.COMPLETE
        observations = native_command_observations()
        for name, (data, status, count, limit) in reference.COMMAND_LAYOUT.items():
            sample = observations[name]
            self.assertLessEqual(len(sample["bytes"]), limit)
            mailbox[count] = len(sample["bytes"])
            for address, key in ((data, "bytes"), (status, "kernal_st")):
                start = address - reference.CAPTURE_ADDRESS
                capture[start:start + len(sample[key])] = bytes(sample[key])
        self.assertEqual(reference.decode_capture(capture, mailbox, reference.COMMAND_LAYOUT),
                         observations)

    def test_program_reserves_capture_ram_and_bounds_timeout(self):
        """Keep BASIC's heap out of captures and restore its input timeout setting."""
        program = reference.command_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        self.assertIn("bo=peek(1020):poke1020,0", program)
        self.assertIn("poke1020,bo", program)
        self.assertIn('print#15,"u";:', program)
        for phase in reference.COMMAND_PHASES:
            if phase:
                self.assertIn(f"poke{reference.PHASE_ADDRESS},{phase}:", program)


class RenameObservationTest(unittest.TestCase):
    """Require complete rename captures and reject false success or missing EOF."""

    def observations(self, model):
        """Encode source-derived status classes independently of the capture decoder."""
        messages = {0: "00, OK", 30: "30,SYNTAX ERROR", 34: "34,SYNTAX ERROR",
                    62: "62, FILE NOT FOUND", 63: "63, FILE EXISTS",
                    73: "73,CBM DOS V2" if model == "4040" else "73,CBM DOS V2.7"}
        result = {}
        for name, _, channel, _, expected in reference.RENAME_STEPS:
            payload = ((messages[expected] + (",00,00\r" if model == "4040" else ",00,00,0\r"))
                       .encode("ascii") if channel == 15 else expected)
            result[name] = {
                "bytes": list(payload),
                "kernal_st": [0] * (len(payload) - 1) + [64] if channel == 15 else [0] * len(payload),
            }
        return result

    def test_complete_models(self):
        """Retain exact 4040 versus DOS 2.7 status punctuation and drive suffix."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_rename_observations(model, self.observations(model))

    def test_incomplete_capture_is_not_success(self):
        """Do not silently omit a malformed-input or writer lifecycle observation."""
        observations = self.observations("4040")
        del observations["rename_writer_close"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_rename_observations("4040", observations)

    def test_wrong_error_precedence_fails(self):
        """An existing target does not outrank an absent source."""
        observations = self.observations("4040")
        observations["rename_missing_precedence"] = observations["rename_collision"]
        with self.assertRaisesRegex(ValueError, "rename_missing_precedence"):
            reference.check_rename_observations("4040", observations)

    def test_rewound_or_modified_data_fails(self):
        """A renamed file retains both its original data and an open writer's data."""
        for name in ("rename_prefix", "rename_writer_prefix"):
            observations = self.observations("4040")
            observations[name]["bytes"][0] ^= 1
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_rename_observations("4040", observations)

    def test_status_eoi_is_required(self):
        """A matching status string without its real final EOI is incomplete."""
        observations = self.observations("4040")
        observations["rename_success"]["kernal_st"][-1] = 0
        with self.assertRaisesRegex(ValueError, "rename_success"):
            reference.check_rename_observations("4040", observations)

    def test_capture_layout_and_program_are_bounded(self):
        """Keep every observation and count inside the existing capture/mailbox."""
        program = reference.rename_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        for name, (data, status, count, limit) in reference.RENAME_LAYOUT.items():
            with self.subTest(name=name):
                self.assertGreater(count, 0)
                self.assertLess(count, reference.PHASE_ADDRESS - reference.MAILBOX_ADDRESS)
                self.assertLess(status + limit, reference.CAPTURE_ADDRESS + 0x1c00)
                self.assertLess(data + limit, status + 1)


class CopyObservationTest(unittest.TestCase):
    """Reject false copy success, incomplete cases and changed peer cursors."""

    def observations(self, model):
        """Construct native status lines independently of the capture decoder."""
        messages = {0: "00, OK", 30: "30,SYNTAX ERROR", 62: "62, FILE NOT FOUND",
                    63: "63, FILE EXISTS", 64: "64, FILE TYPE MISMATCH",
                    73: "73,CBM DOS V2" if model == "4040" else "73,CBM DOS V2.7"}
        result = {}
        for name, _, channel, _, expected in reference.COPY_STEPS:
            payload = ((messages[expected] + (",00,00\r" if model == "4040" else ",00,00,0\r"))
                       .encode("ascii") if channel == 15 else expected)
            result[name] = {
                "bytes": list(payload),
                "kernal_st": [0] * (len(payload) - 1) + [64] if channel == 15 else [0] * len(payload),
            }
        return result

    def test_complete_models(self):
        """Keep all selected ROM status formats and every expected observation."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_copy_observations(model, self.observations(model))

    def test_incomplete_capture_fails(self):
        """Do not count a subset of copy operations as complete evidence."""
        observed = self.observations("4040")
        del observed["copy_append"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_copy_observations("4040", observed)

    def test_missing_source_precedes_existing_target(self):
        """Native CHKIN checks every source before CHKIO tests the destination."""
        observed = self.observations("4040")
        observed["copy_missing_precedence"] = observed["copy_collision"]
        with self.assertRaisesRegex(ValueError, "copy_missing_precedence"):
            reference.check_copy_observations("4040", observed)

    def test_cursor_and_eoi_are_not_normalized(self):
        """Reject a rewound peer or a status without final EOI."""
        for name, key in (("copy_peer_resume", "bytes"), ("copy_seed", "kernal_st")):
            observed = self.observations("4040")
            observed[name][key][-1] ^= 1
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_copy_observations("4040", observed)

    def test_capture_layout_and_program_are_bounded(self):
        """Keep the generated controller inside the shared observation allocation."""
        program = reference.copy_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        for name, (data, status, count, limit) in reference.COPY_LAYOUT.items():
            with self.subTest(name=name):
                self.assertGreater(count, 0)
                self.assertLess(count, reference.PHASE_ADDRESS - reference.MAILBOX_ADDRESS)
                self.assertLess(status + limit, reference.CAPTURE_ADDRESS + 0x1c00)
                self.assertLess(data + limit, status + 1)
        self.assertEqual(reference.COPY_PAYLOADS["single,s"], b"A")
        self.assertEqual(reference.COPY_PAYLOADS["join,s"], bytes(range(254)) + b"AA")


class DirectObservationTest(unittest.TestCase):
    """Protect raw binary observations, native EOI boundaries and capture limits."""

    def observations(self, model, variant="ordinary"):
        """Construct the primary-source cases independently of the checker."""
        result = {}
        for name, _, channel, _, expected, eoi in reference.DIRECT_STEPS:
            if channel == 15:
                message = {0: " OK", 70: "NO CHANNEL",
                           73: "CBM DOS V2" if model == "4040" else "CBM DOS V2.7"}[expected]
                payload = (f"{expected:02d},{message},00,00"
                           + ("\r" if model == "4040" else ",0\r")).encode("ascii")
            else:
                payload = expected
            status = [0] * len(payload)
            if eoi:
                status[-1] = 64
            result[name] = {"bytes": list(payload), "kernal_st": status}
        if variant == "load":
            result["direct_tail"] = {"bytes": [1] + list(range(1, 254)), "kernal_st": [0] * 253 + [64]}
            result["direct_repeat"] = {"bytes": [255, 255] + list(range(253)), "kernal_st": [0] * 255}
        return result

    def test_complete_models(self):
        """Require every numeric direct-buffer observation on all approved models."""
        for model in reference.MODELS:
            reference.check_direct_observations(model, self.observations(model))

    def test_reject_partial_coverage(self):
        """Do not accept a successful subset of the direct scenario."""
        observed = self.observations("4040")
        del observed["direct_repeat"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_direct_observations("4040", observed)

    def test_reject_ascii_page_number_and_missing_eoi(self):
        """A decimal string or lost sticky EOI is not a native binary result."""
        for name, field, value in (("direct_number", "bytes", [49, 49]),
                                   ("direct_put_wrap", "kernal_st", [0])):
            observed = self.observations("8050")
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_direct_observations("8050", observed)

    def test_binary_cr_and_layout_bounds(self):
        """Binary CR remains data and each sample/status pair has its own space."""
        program = reference.direct_program()
        self.assertIn("(f<>15orv<>13)", program)
        self.assertIn(13, self.observations("4040")["direct_tail"]["bytes"])
        self.assertLess(reference.DIRECT_PHASE_START + len(reference.DIRECT_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.DIRECT_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_special_load_and_save_secondaries(self):
        """Require SA0's duplicated prepared byte without changing SA1's circular GET."""
        for model in reference.MODELS:
            for variant in ("load", "save"):
                with self.subTest(model=model, variant=variant):
                    reference.check_direct_observations(model, self.observations(model, variant), variant)
                    program = reference.direct_program(variant)
                    automatic, explicit = (0, 1) if variant == "load" else (1, 0)
                    self.assertIn(f'open2,8,{automatic},"#"', program)
                    self.assertIn(f'open4,8,{explicit},"#0"', program)

    def test_load_cannot_be_normalized_to_ordinary_direct(self):
        """Reject lost duplicates, premature EOI and incomplete special-channel captures."""
        for name, field, value in (("direct_tail", "bytes", list(range(1, 255))),
                                   ("direct_tail", "kernal_st", [0] * 254),
                                   ("direct_repeat", "kernal_st", [0] * 254 + [64])):
            observed = self.observations("4040", "load")
            observed[name][field] = value
            with self.subTest(name=name, field=field), self.assertRaisesRegex(ValueError, name):
                reference.check_direct_observations("4040", observed, "load")
        observed = self.observations("8050", "load")
        del observed["direct_repeat"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_direct_observations("8050", observed, "load")


class PointerObservationTest(unittest.TestCase):
    """Protect the B-P binary capture, exact errors and complete scenario coverage."""

    def observations(self, model):
        """Build expected samples separately from the production checker."""
        result = {}
        for name, _, channel, _, expected, eoi in reference.POINTER_STEPS:
            if channel == 15:
                message = {30: "SYNTAX ERROR", 70: "NO CHANNEL",
                           73: "CBM DOS V2" if model == "4040" else "CBM DOS V2.7"}[expected]
                payload = (f"{expected:02d},{message},00,00"
                           + ("\r" if model == "4040" else ",0\r")).encode("ascii")
            else:
                payload = expected
            status = [0] * len(payload)
            if eoi:
                status[-1] = 64
            result[name] = {"bytes": list(payload), "kernal_st": status}
        return result

    def test_complete_models(self):
        """Require all numeric pointer cases on every approved ROM model."""
        for model in reference.MODELS:
            reference.check_pointer_observations(model, self.observations(model))

    def test_missing_case_fails(self):
        """Reject a successful prefix rather than hiding an omitted peer continuation."""
        observed = self.observations("4040")
        del observed["pointer_peer_retained"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_pointer_observations("4040", observed)

    def test_wrong_offset_eoi_and_status_fail(self):
        """Pointer zero, sticky EOI and native syntax codes are independently required."""
        for name, field, value in (("pointer_zero", "bytes", [0]),
                                   ("pointer_sticky", "kernal_st", [0]),
                                   ("pointer_fifth", "bytes", list(b"00, OK,00,00\r"))):
            observed = self.observations("4040")
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_pointer_observations("4040", observed)

    def test_binary_cr_and_bounded_capture(self):
        """Preserve raw CR and keep every observation inside the fixed capture allocation."""
        self.assertIn("(f<>15orv<>13)", reference.pointer_program())
        self.assertEqual(self.observations("4040")["pointer_four"]["bytes"], [13, 14])
        self.assertLess(reference.POINTER_PHASE_START + len(reference.POINTER_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.POINTER_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)


class BlockReadObservationTest(unittest.TestCase):
    """Protect native B-R/U1 boundaries, model-specific diagnostics and fixture ownership."""

    def observations(self, model, steps=reference.BLOCK_READ_STEPS):
        """Construct independent data/status samples for the complete block-read scenario."""
        result = {}
        for name, _, channel, _, expected, eoi in steps:
            if channel == 15:
                code, track, sector = expected[:3] if isinstance(expected, tuple) else (expected, 0, 0)
                drive = expected[3] if isinstance(expected, tuple) and len(expected) == 4 else 0
                message = {0: " OK", 30: "SYNTAX ERROR", 31: "SYNTAX ERROR", 34: "SYNTAX ERROR", 63: " FILE EXISTS", 65: "NO BLOCK", 66: "ILLEGAL TRACK OR SECTOR", 70: "NO CHANNEL",
                           73: "CBM DOS V2" if model == "4040" else "CBM DOS V2.7"}[code]
                coordinates = (("55,55" if model == "4040" else "255,255") if code == 66
                               else f"{track:02d},{sector:02d}")
                payload = (f"{code:02d},{message},{coordinates}"
                           + ("\r" if model == "4040" else f",{drive}\r")).encode("ascii")
            else:
                payload = expected
            status = [0] * len(payload)
            if eoi:
                status[-1] = 64
            result[name] = {"bytes": list(payload), "kernal_st": status}
        return result

    def test_complete_models(self):
        """Require complete original-ROM cases for every approved model."""
        for model in reference.MODELS:
            reference.check_block_read_observations(model, self.observations(model))

    def test_reject_missing_tail(self):
        """A missing 255-byte raw tail is incomplete evidence."""
        observed = self.observations("4040")
        del observed["block_read_raw_tail"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_block_read_observations("4040", observed)

    def test_wrong_count_raw_offset_and_coordinates_fail(self):
        """Do not normalize B-R/U1 offset, EOI or the 4040's truncated coordinates."""
        for name, field, value in (("block_read_counted", "kernal_st", [0] * 4),
                                   ("block_read_raw", "bytes", [13]),
                                   ("block_read_invalid", "bytes",
                                    list(b"66,ILLEGAL TRACK OR SECTOR,255,255\r"))):
            observed = self.observations("4040")
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_block_read_observations("4040", observed)

    def test_binary_cr_and_capture_bounds(self):
        """Preserve raw binary CR and fit every observation inside the fixed dump."""
        self.assertIn("(f<>15orv<>13)", reference.block_read_program())
        self.assertEqual(self.observations("4040")["block_read_counted"]["bytes"], [13, 0, 255, 42])
        self.assertLess(reference.BLOCK_READ_PHASE_START + len(reference.BLOCK_READ_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.BLOCK_READ_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_fixture_changes_only_free_sector(self):
        """Seed sector 1/0 without modifying any BAM or directory byte."""
        for model in reference.MODELS:
            size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
            data = bytearray(b"\x5a" * size)
            bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
            bitmap = bam + (5 if model == "4040" else 7)
            data[bitmap] |= 1
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "fixture.img"
                path.write_bytes(data)
                reference.block_read_fixture(model, path)
                after = path.read_bytes()
                self.assertEqual(after[:5], bytes((4, 13, 0, 255, 42)))
                self.assertEqual(after[255], 99)
                self.assertEqual(after[256:], data[256:])

    def test_fixture_refuses_allocated_sector(self):
        """An allocated fixture sector must not be overwritten."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.d64"
            original = bytes(174848)
            path.write_bytes(original)
            with self.assertRaisesRegex(ValueError, "allocated"):
                reference.block_read_fixture("4040", path)
            self.assertEqual(path.read_bytes(), original)


class BlockWriteObservationTest(unittest.TestCase):
    """Protect native B-W/U2 state and exact full-image persistence evidence."""

    def observations(self, model):
        """Construct block-write samples using the shared independent status encoder."""
        return BlockReadObservationTest().observations(model, reference.BLOCK_WRITE_STEPS)

    def test_complete_models(self):
        """Require complete write observations for all approved ROMs."""
        for model in reference.MODELS:
            reference.check_block_write_observations(model, self.observations(model))

    def test_missing_case_is_not_success(self):
        """A missing final maximum-count write cannot establish persistence."""
        observed = self.observations("4040")
        del observed["block_write_maximum"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_block_write_observations("4040", observed)

    def test_counted_and_raw_eoi_are_not_normalized(self):
        """B-W preserves LSTCHR while U2 retains the pending EOI on byte zero."""
        for name in ("block_write_counted", "block_write_raw_state"):
            observed = self.observations("4040")
            observed[name]["kernal_st"][-1] ^= 64
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_block_write_observations("4040", observed)

    def test_capture_is_bounded(self):
        """Keep binary capture areas disjoint and every phase below completion."""
        self.assertIn("(f<>15orv<>13)", reference.block_write_program())
        self.assertLess(reference.BLOCK_WRITE_PHASE_START + len(reference.BLOCK_WRITE_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.BLOCK_WRITE_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_full_media_and_unused_tail(self):
        """Require the exact destination, unchanged metadata and every unused tail byte."""
        for model in reference.MODELS:
            size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
            image = bytearray(size)
            bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
            image[bam + (5 if model == "4040" else 7)] = 3
            with tempfile.TemporaryDirectory() as directory:
                initial, final = Path(directory) / "initial.img", Path(directory) / "final.img"
                initial.write_bytes(image)
                reference.block_write_fixture(model, initial)
                image = bytearray(initial.read_bytes())
                self.assertEqual(image[:5], bytes((4, 13, 0, 255, 42)))
                self.assertEqual(image[256:512], bytes(256))
                image[256:512] = bytes((254, 13, 0, 255, 42)) + bytes(range(5, 255)) + bytes((99,))
                final.write_bytes(image)
                reference.check_block_write_media(model, initial, final)
                for offset in (511, size - 1):
                    wrong = bytearray(image)
                    wrong[offset] ^= 1
                    final.write_bytes(wrong)
                    with self.assertRaisesRegex(ValueError, "persisted raw page"):
                        reference.check_block_write_media(model, initial, final)

    def test_fixture_refuses_allocated_destination(self):
        """Do not modify even the source when the destination is already allocated."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "initial.d64"
            image = bytearray(174848)
            image[reference.image_offset("4040", 18, 0) + 5] = 1
            path.write_bytes(image)
            with self.assertRaisesRegex(ValueError, "must be free"):
                reference.block_write_fixture("4040", path)
            self.assertEqual(path.read_bytes(), image)


class UserInterruptObservationTest(unittest.TestCase):
    """Protect IRQ-entry utility observations and exact retained-map persistence."""

    def observations(self, model):
        """Construct independent status and binary payload samples."""
        return BlockReadObservationTest().observations(model, reference.USER_INTERRUPT_STEPS)

    def test_complete_models(self):
        """Require complete native IRQ cases for every selected model."""
        for model in reference.MODELS:
            reference.check_user_interrupt_observations(model, self.observations(model))

    def test_missing_case_is_not_success(self):
        """Do not accept an omitted stale-selector or final-page observation."""
        for name in ("user_interrupt_short", "user_interrupt_reuse"):
            observed = self.observations("4040")
            del observed[name]
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "coverage"):
                reference.check_user_interrupt_observations("4040", observed)

    def test_reset_status_data_or_eoi_are_not_irq_entry(self):
        """A reset banner, restarted peer or sticky direct EOI is a real difference."""
        for name, field, value in (("user_interrupt_vector", "bytes", list(b"73,CBM DOS V2,00,00\r")),
                                   ("user_interrupt_short", "bytes", list(b"30,SYNTAX ERROR,00,00\r")),
                                   ("user_interrupt_page", "kernal_st", [0, 0, 0, 64]),
                                   ("user_interrupt_peer_retained", "bytes", list(reference.SEED[:4]))):
            observed = self.observations("4040")
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_user_interrupt_observations("4040", observed)

    def test_capture_bounds_and_nonreturning_aliases(self):
        """Retain the semicolon selector and no-CR short U within the complete dump."""
        program = reference.user_interrupt_program()
        for command in ('"uk"', '"u;"', '"u["', '"u";'):
            self.assertIn(command, program)
        self.assertLess(reference.USER_INTERRUPT_PHASE_START + len(reference.USER_INTERRUPT_CASES),
                        reference.COMPLETE)
        for data, status, mailbox, capacity in reference.USER_INTERRUPT_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_exact_persisted_claim_on_every_model(self):
        """Unlike warm reset, DOS 2.7 IRQ re-entry must retain and publish the claim."""
        for model in reference.MODELS:
            size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
            image = bytearray(size)
            bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
            if model != "4040":
                image[bam + 4:bam + 6] = bytes((1, 51))
            entry = bam + (4 if model == "4040" else 6)
            image[entry:entry + 2] = bytes((1, 1))
            with tempfile.TemporaryDirectory() as directory:
                initial, final = Path(directory) / "initial.img", Path(directory) / "final.img"
                initial.write_bytes(image)
                final.write_bytes(image)
                with self.assertRaisesRegex(ValueError, "retained BAM claim"):
                    reference.check_user_interrupt_media(model, initial, final)
                image[entry:entry + 2] = bytes(2)
                final.write_bytes(image)
                reference.check_user_interrupt_media(model, initial, final)
                image[-1] ^= 1
                final.write_bytes(image)
                with self.assertRaisesRegex(ValueError, "retained BAM claim"):
                    reference.check_user_interrupt_media(model, initial, final)
                initial.write_bytes(image)
                with self.assertRaisesRegex(ValueError, "must be free"):
                    reference.check_user_interrupt_media(model, initial, final)


class FormatObservationTest(unittest.TestCase):
    """Protect NEW/DUPLICATE's complete observations and independently built images."""

    def test_complete_models_and_variants(self):
        """Require exact native status, header rows and EOI for all nine scenarios."""
        for model in reference.MODELS:
            for variant in ("quick", "full", "duplicate"):
                steps = reference.format_steps(model, variant)
                observations = BlockReadObservationTest().observations(model, steps)
                reference.check_block_observations(model, observations, steps, reference.format_layout(variant),
                                                   "format")

    def test_missing_reopened_listing_is_not_success(self):
        """An early successful command cannot replace the final cache-reload observation."""
        for variant in ("quick", "full", "duplicate"):
            steps = reference.format_steps("8050", variant)
            observations = BlockReadObservationTest().observations("8050", steps)
            del observations["format_reopened"]
            with self.assertRaisesRegex(ValueError, "coverage"):
                reference.check_block_observations("8050", observations, steps, reference.format_layout(variant),
                                                   "format")

    def test_quick_preserves_data_and_id_but_full_clears_them(self):
        """Check raw unused data and header bytes without using firmware helpers."""
        for model, size in (("4040", 174848), ("8050", 533248), ("8250", 1066496)):
            initial = bytearray(b"\x5a" * size)
            header = reference.image_offset(model, 18 if model == "4040" else 39, 0)
            label = 144 if model == "4040" else 6
            initial[header + label + 18:header + label + 20] = b"XY"
            quick = reference.format_expected_image(model, "quick", initial)
            full = reference.format_expected_image(model, "full", initial)
            self.assertEqual(quick[:256], b"\x5a" * 256)
            self.assertEqual(full[:256], bytes(256))
            self.assertEqual(quick[header + label + 18:header + label + 20], b"XY")
            self.assertEqual(full[header + label + 18:header + label + 20], b"AB")
            self.assertEqual(full[header + label:header + label + 16], b"M4 FORMATTED\xa0\xa0\xa0\xa0")
            directory = reference.image_offset(model, 18 if model == "4040" else 39, 1)
            self.assertEqual(full[directory:directory + 256], b"\0\xff" + bytes(254))

    def test_complete_image_mutations_fail(self):
        """Reject a wrong byte anywhere, not just the formatted header or BAM."""
        with tempfile.TemporaryDirectory() as directory:
            initial, final = Path(directory) / "initial", Path(directory) / "final"
            initial.write_bytes(bytes(174848))
            for variant in ("quick", "full"):
                data = reference.format_expected_image("4040", variant, initial.read_bytes())
                final.write_bytes(data)
                reference.check_format_media("4040", variant, initial, final)
                final.write_bytes(data[:-1] + bytes((data[-1] ^ 1,)))
                with self.assertRaisesRegex(ValueError, "complete persisted image"):
                    reference.check_format_media("4040", variant, initial, final)

    def test_duplicate_requires_the_entire_source(self):
        """Free-sector bytes are part of sector DUPLICATE, not only named files."""
        with tempfile.TemporaryDirectory() as directory:
            source, final = Path(directory) / "source", Path(directory) / "final"
            source.write_bytes(bytes(1023) + b"\x5a")
            final.write_bytes(source.read_bytes())
            reference.check_format_media("4040", "duplicate", source, final, source)
            final.write_bytes(bytes(1024))
            with self.assertRaisesRegex(ValueError, "complete persisted image"):
                reference.check_format_media("4040", "duplicate", source, final, source)

    def test_program_layout_is_complete_and_bounded(self):
        """Keep all executed records in protected capture RAM and the mailbox."""
        for variant in ("quick", "full", "duplicate"):
            steps = reference.format_steps("4040", variant)
            layout = reference.format_layout(variant)
            program = reference.binary_program(steps, layout, reference.FORMAT_PHASE_START)
            self.assertEqual(set(layout), {step[0] for step in steps})
            self.assertIn('print#15,"d0=1"' if variant == "duplicate" else 'print#15,"n0:m4 formatted', program)
            for data, status, mailbox, capacity in layout.values():
                self.assertGreaterEqual(data, reference.CAPTURE_ADDRESS)
                self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
                self.assertLess(mailbox, 31)


class CopyAllObservationTest(unittest.TestCase):
    """Protect whole-drive syntax, complete captures and source-media immutability."""

    def test_complete_models(self):
        """Require complete binary/status observations for every selected model."""
        for model in reference.MODELS:
            observed = BlockReadObservationTest().observations(model, reference.COPY_ALL_STEPS)
            reference.check_copy_all_observations(model, observed)

    def test_missing_recovery_is_not_success(self):
        """A successful prefix cannot replace the final named-copy recovery."""
        observed = BlockReadObservationTest().observations("4040", reference.COPY_ALL_STEPS)
        del observed["copy_all_recovery"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_copy_all_observations("4040", observed)

    def test_native_status_spacing_and_selected_drive_are_exact(self):
        """Retain FILE EXISTS's leading space and the source drive at final ENDCMD."""
        for name, payload in (
            ("copy_all_exists", b"63, FILE EXISTS,00,00,0\r"),
            ("copy_all_success", b"00, OK,00,00,1\r"),
        ):
            observed = BlockReadObservationTest().observations("8050", reference.COPY_ALL_STEPS)
            self.assertEqual(bytes(observed[name]["bytes"]), payload)
            observed[name]["bytes"] = list(payload.replace(b", FILE", b",FILE").replace(b",1\r", b",0\r"))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_copy_all_observations("8050", observed)

    def test_repeated_copy_and_boundary_eoi_are_exact(self):
        """Do not normalize repeated-copy failure, copied data or a boundary's EOI."""
        for name, field, value in (
            ("copy_all_exists", "bytes", list(b"00, OK,00,00\r")),
            ("copy_all_one", "bytes", list(b"AAAA")),
            ("copy_all_bound255", "kernal_st", [0] * 255),
            ("copy_all_peer_retained", "bytes", list(reference.SEED[:4])),
        ):
            observed = BlockReadObservationTest().observations("4040", reference.COPY_ALL_STEPS)
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_copy_all_observations("4040", observed)

    def test_source_full_image_is_immutable(self):
        """A mutation in an unused source tail must fail, not only changed payloads."""
        with tempfile.TemporaryDirectory() as directory:
            initial, final = Path(directory) / "initial", Path(directory) / "final"
            initial.write_bytes(bytes(1024))
            final.write_bytes(initial.read_bytes())
            reference.check_copy_all_source(initial, final)
            final.write_bytes(bytes(1023) + b"\x01")
            with self.assertRaisesRegex(ValueError, "complete source"):
                reference.check_copy_all_source(initial, final)

    def test_program_layout_is_bounded(self):
        """Keep all fourteen records, including 255-byte output, inside protected RAM."""
        program = reference.copy_all_program()
        self.assertIn('print#15,"c0=1"', program)
        self.assertIn('open3,8,3,"1:seed,p,r"', program)
        self.assertEqual(len(reference.COPY_ALL_STEPS), 14)
        for data, status, mailbox, capacity in reference.COPY_ALL_LAYOUT.values():
            self.assertGreaterEqual(data, reference.CAPTURE_ADDRESS)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)
            self.assertEqual(capacity, 255)
        self.assertLess(reference.COPY_ALL_PHASE_START + len(reference.COPY_ALL_STEPS), 255)


class CopyPatternObservationTest(unittest.TestCase):
    """Protect CPYDTD patterns, native model differences and complete binary records."""

    def test_complete_models(self):
        """Require all fourteen independently encoded observations for each model."""
        for model in reference.MODELS:
            steps = reference.copy_all_steps(model, "pattern")
            observed = BlockReadObservationTest().observations(model, steps)
            reference.check_copy_all_observations(model, observed, "pattern")

    def test_model_specific_same_drive_status_is_exact(self):
        """4040 reaches FILE EXISTS where DOS 2.7 rejects the explicit equal drives."""
        for model in reference.MODELS:
            observed = BlockReadObservationTest().observations(
                model, reference.copy_all_steps(model, "pattern"))
            expected = (b"63, FILE EXISTS,00,00\r" if model == "4040"
                        else b"30,SYNTAX ERROR,00,00,1\r")
            self.assertEqual(bytes(observed["copy_pattern_same"]["bytes"]), expected)
            observed["copy_pattern_same"]["bytes"][0] ^= 1
            with self.assertRaisesRegex(ValueError, "copy_pattern_same"):
                reference.check_copy_all_observations(model, observed, "pattern")

    def test_missing_recovery_is_incomplete(self):
        """Do not accept a captured patterned prefix without the later named COPY."""
        observed = BlockReadObservationTest().observations(
            "4040", reference.copy_all_steps("4040", "pattern"))
        del observed["copy_pattern_recovery"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_copy_all_observations("4040", observed, "pattern")

    def test_payload_eoi_peer_and_no_match_status_are_exact(self):
        """Reject payload normalization, lost final EOI and accidental source advancement."""
        for name, field, value in (
            ("copy_pattern_one", "bytes", list(b"AAAA")),
            ("copy_pattern_bound255", "kernal_st", [0] * 255),
            ("copy_pattern_peer_retained", "bytes", list(reference.SEED[:4])),
            ("copy_pattern_missing", "bytes", list(b"62, FILE NOT FOUND,00,00\r")),
        ):
            observed = BlockReadObservationTest().observations(
                "4040", reference.copy_all_steps("4040", "pattern"))
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_copy_all_observations("4040", observed, "pattern")

    def test_program_layout_is_bounded_and_distinct(self):
        """Retain every final observation inside the protected capture/mailbox regions."""
        program = reference.copy_all_program("8050", "pattern")
        self.assertIn('print#15,"c0:*ignored=1:one"', program)
        self.assertIn('print#15,"c0:*=1:bound25?"', program)
        self.assertEqual(len(reference.COPY_PATTERN_STEPS), 14)
        self.assertNotEqual(reference.copy_all_layout(), reference.copy_all_layout("pattern"))
        for data, status, mailbox, capacity in reference.copy_all_layout("pattern").values():
            self.assertGreaterEqual(data, reference.CAPTURE_ADDRESS)
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)
            self.assertEqual(capacity, 255)


class ReaderPointerObservationTest(unittest.TestCase):
    """Protect native pointer duplication, timeout/EOI and immutable raw expectations."""

    def fixture(self, model, directory):
        """Create independent directory/page bytes with a non-default link and raw tail."""
        size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
        data = bytearray(size)
        root = reference.image_offset(model, 18 if model == "4040" else 39, 1)
        for index, name in enumerate((b"seed", b"one")):
            entry = root + index * 32
            data[entry + 2:entry + 5] = bytes((0x82, 2, index))
            data[entry + 5:entry + 21] = name.ljust(16, b"\xa0")
        first = reference.image_offset(model, 2, 0)
        data[first:first + 256] = bytes((5, 0)) + reference.SEED[:254]
        last = reference.image_offset(model, 5, 0)
        data[last:last + 9] = bytes((0, 8)) + reference.SEED[254:]
        data[reference.image_offset(model, 2, 1) + 255] = 0xcc
        bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
        if model == "4040":
            data[bam + 5] = 0x1f
        else:
            data[bam + 4:bam + 8] = bytes((1, 51, 5, 0x1f))
        path = Path(directory) / "initial.img"
        path.write_bytes(data)
        return path

    def observations(self, model, variant):
        """Encode independent fixture link/tail values and PET-generated timeout CR."""
        replacements = {"link": b"\x05", "tail": b"\xcc\xcc", "write_head": b"\0\0"}
        steps = [(name, setup, channel, count, replacements.get(expected, expected), eoi)
                 for name, setup, channel, count, expected, eoi
                 in reference.READER_POINTER_STEPS[variant] if expected is not None]
        result = BlockReadObservationTest().observations(model, steps)
        for name, _, _, _, expected, _ in reference.READER_POINTER_STEPS[variant]:
            if expected is None:
                result[name] = {"bytes": [13], "kernal_st": [2]}
        return result

    def test_complete_models_and_variants(self):
        """Require complete linked/terminal records on all selected models."""
        with tempfile.TemporaryDirectory() as directory:
            for model in reference.MODELS:
                initial = self.fixture(model, directory)
                for variant in reference.READER_POINTER_STEPS:
                    reference.check_reader_pointer_observations(
                        model, variant, self.observations(model, variant), initial)

    def test_missing_suffix_is_not_success(self):
        """The final revived byte must be observed rather than inferred from a prefix."""
        with tempfile.TemporaryDirectory() as directory:
            initial = self.fixture("4040", directory)
            for variant in reference.READER_POINTER_STEPS:
                observed = self.observations("4040", variant)
                del observed[reference.READER_POINTER_STEPS[variant][-1][0]]
                with self.assertRaisesRegex(ValueError, "coverage"):
                    reference.check_reader_pointer_observations("4040", variant, observed, initial)

    def test_duplication_sticky_eoi_and_timeout_are_exact(self):
        """Normalized data or drive-EOI substituted for timeout cannot establish parity."""
        substitutions = (
            ("block", "reader_block_counted", "bytes", list(reference.BLOCK_READ_PAGE[1:5])),
            ("block", "reader_block_raw", "bytes", list(reference.BLOCK_READ_PAGE[:4])),
            ("block", "reader_block_raw_tail", "kernal_st", [0] * 253),
            ("block", "reader_block_raw_exhausted", "kernal_st", [64]),
            ("linked", "reader_pointer_duplicate", "bytes", [0, 1, 2, 3]),
            ("linked", "reader_pointer_active", "bytes", [0, 0, 1]),
            ("terminal", "reader_pointer_sticky", "kernal_st", [0]),
            ("terminal", "reader_pointer_tail", "kernal_st", [0, 0]),
            ("terminal", "reader_pointer_exhausted", "kernal_st", [64]),
        )
        with tempfile.TemporaryDirectory() as directory:
            initial = self.fixture("4040", directory)
            for variant, name, field, value in substitutions:
                observed = self.observations("4040", variant)
                observed[name][field] = value
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                    reference.check_reader_pointer_observations("4040", variant, observed, initial)

    def test_fixture_link_tail_and_media_identity_are_independent(self):
        """Read raw link/tail bytes, reject missing entries and compare every image byte."""
        with tempfile.TemporaryDirectory() as directory:
            initial = self.fixture("4040", directory)
            self.assertEqual(reference.reader_pointer_values("4040", initial),
                             {"link": b"\x05", "tail": b"\xcc\xcc"})
            final = Path(directory) / "final.img"
            final.write_bytes(initial.read_bytes())
            reference.check_reader_pointer_media(initial, final)
            changed = bytearray(final.read_bytes())
            changed[-1] ^= 1
            final.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, "complete persisted"):
                reference.check_reader_pointer_media(initial, final)
            initial.write_bytes(bytes(len(changed)))
            with self.assertRaisesRegex(ValueError, "fixture"):
                reference.reader_pointer_values("4040", initial)

    def test_program_layout_is_bounded(self):
        """Keep both complete 14-slot captures within the dump and phase mailboxes."""
        for variant in reference.READER_POINTER_STEPS:
            program = reference.reader_pointer_program(variant)
            self.assertIn({"terminal": "b-p:2,255", "linked": "b-p:2,254", "block": "u1:2,0,1,0",
                           "write": "b-w:2,0,1,4"}[variant],
                          program)
            self.assertEqual(len(reference.READER_POINTER_STEPS[variant]), 14)
            for data, status, mailbox, capacity in reference.READER_POINTER_LAYOUTS[variant].values():
                self.assertGreaterEqual(data, reference.CAPTURE_ADDRESS)
                self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
                self.assertLess(data, status)
                self.assertLess(mailbox, 31)
            self.assertLess(reference.READER_POINTER_PHASE_START + 14, 255)

    def test_block_reads_are_not_direct_channel_reads(self):
        """Require sequential duplication, a complete raw page and independent-peer progress."""
        for model in reference.MODELS:
            observed = self.observations(model, "block")
            self.assertEqual(observed["reader_block_counted"]["bytes"], [13, 13, 0, 255, 42])
            self.assertEqual(observed["reader_block_raw"]["bytes"], [4, 4, 13, 0])
            self.assertEqual(observed["reader_block_raw_tail"]["bytes"], list(reference.BLOCK_READ_PAGE[3:]))
            self.assertEqual(observed["reader_block_raw_tail"]["kernal_st"], [0] * 252 + [64])
            self.assertEqual(observed["reader_block_peer_retained"]["bytes"], [2, 3])
            self.assertEqual(observed["reader_block_prefix"]["bytes"], list(reference.SEED[:253]))

    def test_block_reads_require_both_eof_timeouts(self):
        """Counted and raw exhaustion are separately required, not inferred from EOI."""
        with tempfile.TemporaryDirectory() as directory:
            initial = self.fixture("4040", directory)
            for name in ("reader_block_exhausted", "reader_block_raw_exhausted"):
                observed = self.observations("4040", "block")
                del observed[name]
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "coverage"):
                    reference.check_reader_pointer_observations("4040", "block", observed, initial)

    def test_write_media_requires_each_complete_native_page(self):
        """Check count mutation, raw active selection and all bytes outside destinations."""
        with tempfile.TemporaryDirectory() as directory:
            for model in reference.MODELS:
                initial = self.fixture(model, directory)
                reference.reader_write_fixture(model, initial)
                raw = initial.read_bytes()
                first = raw[reference.image_offset(model, 2, 0):reference.image_offset(model, 2, 0) + 256]
                last = raw[reference.image_offset(model, 5, 0):reference.image_offset(model, 5, 0) + 256]
                expected = bytearray(raw)
                for sector, page in enumerate((b"\x06" + first[1:], b"\x06" + first[1:],
                                               last, last, b"\x07" + last[1:])):
                    offset = reference.image_offset(model, 1, sector)
                    expected[offset:offset + 256] = page
                final = Path(directory) / "final.img"
                final.write_bytes(expected)
                reference.check_reader_write_media(model, initial, final)
                for offset in (0, 255, 256, 511, 512, 768, 1024, len(expected) - 1):
                    changed = bytearray(expected)
                    changed[offset] ^= 1
                    final.write_bytes(changed)
                    with self.subTest(model=model, offset=offset), self.assertRaisesRegex(ValueError, "complete"):
                        reference.check_reader_write_media(model, initial, final)
                initial.write_bytes(bytes(len(raw)))
                with self.assertRaisesRegex(ValueError, "fixture"):
                    reference.reader_write_fixture(model, initial)

    def test_write_observations_reject_wrong_pointer_page_and_readiness(self):
        """A normalized cursor, old active page or missing EOF timeout cannot pass."""
        with tempfile.TemporaryDirectory() as directory:
            initial = self.fixture("4040", directory)
            for name, field, value in (
                ("reader_write_counted", "bytes", [0, 1]),
                ("reader_write_raw", "bytes", [0, 0]),
                ("reader_write_active", "bytes", [250, 251]),
                ("reader_write_sticky", "kernal_st", [0]),
                ("reader_write_exhausted", "kernal_st", [64]),
                ("reader_write_peer_retained", "bytes", [0, 1]),
            ):
                observed = self.observations("4040", "write")
                observed[name][field] = value
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                    reference.check_reader_pointer_observations("4040", "write", observed, initial)


class WarmResetObservationTest(unittest.TestCase):
    """Protect warm/cold distinctions, deferred AUTOI and retained page evidence."""

    def observations(self, model):
        """Encode timeout separately from real drive CR/EOI and status bytes."""
        steps = [step for step in reference.warm_reset_steps(model) if step[4] is not None]
        result = BlockReadObservationTest().observations(model, steps)
        result["warm_reset_closed"] = {"bytes": [13], "kernal_st": [2]}
        return result

    def test_complete_models(self):
        """Require complete model-specific warm-reset observations."""
        for model in reference.MODELS:
            reference.check_warm_reset_observations(model, self.observations(model))

    def test_timeout_and_retained_ram_cannot_be_normalized(self):
        """A drive CR or cleared data page is not successful warm-reset evidence."""
        for name, field, value in (("warm_reset_closed", "kernal_st", [64]),
                                   ("warm_reset_page", "bytes", [0] * 4),
                                   ("warm_reset_short", "bytes", list(b"00, OK,00,00\r"))):
            observed = self.observations("4040")
            observed[name][field] = value
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_warm_reset_observations("4040", observed)

    def test_deferred_map_reload_and_missing_cases(self):
        """Keep pre-OPEN retained claims distinct from DOS 2.7's post-OPEN reload."""
        observed = self.observations("8050")
        observed["warm_reset_before_open"]["bytes"] = list(b"00, OK,00,00,0\r")
        with self.assertRaisesRegex(ValueError, "warm_reset_before_open"):
            reference.check_warm_reset_observations("8050", observed)
        observed = self.observations("8050")
        observed["warm_reset_cached_map"]["bytes"] = list(b"65,NO BLOCK,01,01,0\r")
        with self.assertRaisesRegex(ValueError, "warm_reset_cached_map"):
            reference.check_warm_reset_observations("8050", observed)
        del observed["warm_reset_alias_page"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_warm_reset_observations("8050", observed)

    def test_capture_bounds_and_aliases(self):
        """Capture both NMI aliases and stale selector inside the fixed observation dump."""
        program = reference.warm_reset_program()
        for command in ('"ui"', '"u9"', '"u";'):
            self.assertIn(command, program)
        self.assertLess(reference.WARM_RESET_PHASE_START + len(reference.WARM_RESET_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.WARM_RESET_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_exact_model_specific_persistence(self):
        """Only 4040 retains and publishes its claim, without unrelated image changes."""
        for model in reference.MODELS:
            size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
            image = bytearray(size)
            bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
            if model != "4040":
                image[bam + 4:bam + 6] = bytes((1, 51))
            entry = bam + (4 if model == "4040" else 6)
            image[entry:entry + 2] = bytes((1, 1))
            with tempfile.TemporaryDirectory() as directory:
                initial, final = Path(directory) / "initial.img", Path(directory) / "final.img"
                initial.write_bytes(image)
                expected = bytearray(image)
                if model == "4040":
                    expected[entry:entry + 2] = bytes(2)
                final.write_bytes(expected)
                reference.check_warm_reset_media(model, initial, final)
                expected[-1] ^= 1
                final.write_bytes(expected)
                with self.assertRaisesRegex(ValueError, "retained BAM claim"):
                    reference.check_warm_reset_media(model, initial, final)


class WriteVersionObservationTest(unittest.TestCase):
    """Protect version rejection, zero-version acceptance and immutable rejected images."""

    def test_complete_models_and_versions(self):
        """Check all native models with foreign, zero and matching cached versions."""
        for model in reference.MODELS:
            for version in ("foreign", "zero", "native"):
                observations = BlockReadObservationTest().observations(
                    model, reference.write_version_steps(version))
                reference.check_write_version_observations(model, version, observations)

    def test_reject_normalized_coordinates_and_prepared_byte(self):
        """A startup-shaped 73 or successful B-W prefetch cannot mask a rejected job."""
        for name, payload in (("write_version_counted", b"73,CBM DOS V2,00,00\r"),
                              ("write_version_counted_state", bytes((13, 0, 255, 42)))):
            observations = BlockReadObservationTest().observations("4040", reference.WRITE_VERSION_STEPS)
            observations[name]["bytes"] = list(payload)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_write_version_observations("4040", "foreign", observations)
        del observations["write_version_peer_retained"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_write_version_observations("4040", "foreign", observations)

    def test_capture_bounds_and_initialize(self):
        """Explicit initialization selects native cached metadata before any tested write."""
        self.assertIn('print#15,"i0"', reference.write_version_program("foreign"))
        self.assertLess(reference.WRITE_VERSION_PHASE_START + len(reference.WRITE_VERSION_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.WRITE_VERSION_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_exact_images_and_conflicting_directory_header(self):
        """Keep zero/matching BAM versions distinct from the unrelated DOS 2.7 header."""
        for model in reference.MODELS:
            size = {"4040": 174848, "8050": 533248, "8250": 1066496}[model]
            for version in ("foreign", "zero", "native"):
                data = bytearray(size)
                bam = reference.image_offset(model, 18 if model == "4040" else 38, 0)
                data[bam + (5 if model == "4040" else 7)] = 3
                with tempfile.TemporaryDirectory() as directory:
                    initial, final = Path(directory) / "initial.img", Path(directory) / "final.img"
                    initial.write_bytes(data)
                    reference.write_version_fixture(model, version, initial)
                    expected = bytearray(initial.read_bytes())
                    header = reference.image_offset(model, 18 if model == "4040" else 39, 0)
                    value = {"foreign": 255, "zero": 0, "native": ord("A" if model == "4040" else "C")}[version]
                    self.assertEqual(expected[header + 2], value if model == "4040" else 255)
                    for part in range(1 if model == "4040" else 2 if model == "8050" else 4):
                        self.assertEqual(expected[reference.image_offset(
                            model, 18 if model == "4040" else 38, part * 3) + 2], value)
                    if version != "foreign":
                        expected[256:512] = expected[:256]
                    final.write_bytes(expected)
                    reference.check_write_version_media(model, version, initial, final)
                    expected[-1] ^= 1
                    final.write_bytes(expected)
                    with self.assertRaisesRegex(ValueError, "accepted/rejected page"):
                        reference.check_write_version_media(model, version, initial, final)


class BlockMapObservationTest(unittest.TestCase):
    """Protect native suggestions, free idempotence and whole-image BAM persistence."""

    def observations(self, model):
        """Construct independent allocation status and retained channel samples."""
        return BlockReadObservationTest().observations(model, reference.BLOCK_MAP_STEPS)

    def test_complete_models(self):
        """Require complete allocation observations for every approved ROM."""
        for model in reference.MODELS:
            reference.check_block_map_observations(model, self.observations(model))

    def test_missing_and_wrong_successor_fail(self):
        """Neither omitted cases nor normalized 65 coordinates establish native parity."""
        observed = self.observations("4040")
        observed["block_map_successor"]["bytes"] = list(b"65,NO BLOCK,00,00\r")
        with self.assertRaisesRegex(ValueError, "block_map_successor"):
            reference.check_block_map_observations("4040", observed)
        del observed["block_map_exhausted"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_block_map_observations("4040", observed)

    def test_model_programs_and_bounds(self):
        """Use each model's last legal sector and keep captures disjoint and bounded."""
        for model, coordinate in (("4040", "35,16"), ("8050", "77,22"), ("8250", "154,22")):
            self.assertIn(f'b-a:0,{coordinate}', reference.block_map_program(model))
        self.assertLess(reference.BLOCK_MAP_PHASE_START + len(reference.BLOCK_MAP_CASES), reference.COMPLETE)
        for data, status, mailbox, capacity in reference.BLOCK_MAP_LAYOUT.values():
            self.assertLessEqual(data + capacity, status)
            self.assertLessEqual(status + capacity, reference.CAPTURE_ADDRESS + 0x1c00)
            self.assertLess(mailbox, 31)

    def test_fixture_and_exact_bam_persistence(self):
        """Resolve linked parts, reserve the final sector and reject unrelated image changes."""
        for model, tracks, size in (("4040", 35, 174848), ("8050", 77, 533248), ("8250", 154, 1066496)):
            image = bytearray(size)
            parts = 1 if model == "4040" else (tracks + 49) // 50
            for part in range(parts):
                bam = reference.image_offset(model, 18 if model == "4040" else 38, part * 3)
                if model != "4040":
                    image[bam:bam + 2] = bytes((38, (part + 1) * 3)) if part + 1 < parts else bytes((39, 1))
                    image[bam + 4:bam + 6] = bytes((part * 50 + 1, min((part + 1) * 50 + 1, tracks + 1)))
            first = reference.fixture_bam_entry(model, image, 1)
            image[first:first + 2] = bytes((2, 3))
            last = reference.fixture_bam_entry(model, image, tracks)
            sector = 16 if model == "4040" else 22
            image[last] = 1
            image[last + 1 + sector // 8] = 1 << (sector % 8)
            with tempfile.TemporaryDirectory() as directory:
                initial, final = Path(directory) / "initial.img", Path(directory) / "final.img"
                initial.write_bytes(image)
                reference.block_map_fixture(model, initial)
                expected = bytearray(image)
                expected[last] = expected[last + 1 + sector // 8] = 0
                self.assertEqual(initial.read_bytes(), expected)
                expected[first:first + 2] = bytes((0, 0))
                final.write_bytes(expected)
                reference.check_block_map_media(model, initial, final)
                expected[-1] ^= 1
                final.write_bytes(expected)
                with self.assertRaisesRegex(ValueError, "count/bitmap"):
                    reference.check_block_map_media(model, initial, final)

    def test_allocated_initial_claim_is_rejected(self):
        """Fixture preparation must not release or reinterpret an existing owner."""
        with tempfile.TemporaryDirectory() as directory:
            initial = Path(directory) / "initial.d64"
            original = bytes(174848)
            initial.write_bytes(original)
            with self.assertRaisesRegex(ValueError, "must be free"):
                reference.block_map_fixture("4040", initial)
            self.assertEqual(initial.read_bytes(), original)


class ValidateObservationTest(unittest.TestCase):
    """Protect exact validate outcomes, capture bounds and independent map verification."""

    def observations(self, model):
        """Construct source-derived bytes without using the production decoder."""
        result = {}
        for name, _, channel, _, expected in reference.VALIDATE_STEPS:
            if channel == 15:
                message = {0: " OK", 62: " FILE NOT FOUND",
                           73: "CBM DOS V2" if model == "4040" else "CBM DOS V2.7"}[expected]
                payload = (f"{expected:02d},{message},00,00"
                           + ("\r" if model == "4040" else ",0\r")).encode("ascii")
                status = [0] * (len(payload) - 1) + [64]
            else:
                payload = b"\r" if expected is None else expected
                status = [2] if expected is None else [0] * len(payload)
            result[name] = {"bytes": list(payload), "kernal_st": status}
        return result

    def test_complete_models(self):
        """Require every observation on all three selected ROM profiles."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_validate_observations(model, self.observations(model))

    def test_partial_coverage_fails(self):
        """A successful subset cannot replace the complete scenario."""
        observed = self.observations("4040")
        del observed["validate_abandon"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_validate_observations("4040", observed)

    def test_reader_timeout_and_status_eoi_are_not_normalized(self):
        """Reject false drive-supplied CR and missing status EOI."""
        for name in ("validate_closed", "validate_repair"):
            observed = self.observations("4040")
            observed[name]["kernal_st"][-1] = 0
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_validate_observations("4040", observed)

    def test_abandoned_file_must_be_missing(self):
        """Returning OK after opening the splat is not successful validation."""
        observed = self.observations("8050")
        observed["validate_missing"] = observed["validate_repair"]
        with self.assertRaisesRegex(ValueError, "validate_missing"):
            reference.check_validate_observations("8050", observed)

    def test_layout_and_program_are_bounded(self):
        """Keep each capture, phase and mailbox count inside its allocated range."""
        program = reference.validate_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        self.assertIn('print#15,"validate0"', program)
        for name, (data, status, count, limit) in reference.VALIDATE_LAYOUT.items():
            with self.subTest(name=name):
                self.assertGreater(count, 0)
                self.assertLess(count, reference.PHASE_ADDRESS - reference.MAILBOX_ADDRESS)
                self.assertLess(status + limit, reference.CAPTURE_ADDRESS + 0x1c00)
                self.assertLess(data + limit, status + 1)

    def test_independent_geometry_boundaries(self):
        """Check raw sector geometry at each format zone and the second side."""
        for model, track, count in (("4040", 17, 21), ("4040", 18, 19), ("4040", 25, 18),
                                    ("4040", 31, 17), ("8050", 39, 29), ("8050", 40, 27),
                                    ("8050", 54, 25), ("8050", 65, 23), ("8250", 78, 29)):
            self.assertEqual(reference.image_sectors(model, track), count)
        self.assertEqual(reference.image_offset("8250", 78, 0), 533248)
        for model, track, sector in (("4040", 36, 0), ("8050", 78, 0), ("8250", 155, 0),
                                     ("4040", 18, 19), ("8050", 39, -1)):
            with self.subTest(model=model, track=track, sector=sector), self.assertRaises(ValueError):
                reference.image_offset(model, track, sector)

    def repaired_d64(self):
        """Construct a literal one-file D64 without using the checker geometry helpers."""
        data = bytearray(174848)
        header, directory = 357 * 256, 358 * 256
        data[header:header + 3] = bytes((18, 1, 0x41))
        data[directory:directory + 5] = bytes((0, 255, 0x82, 1, 0))
        data[directory + 5:directory + 21] = b"SEED" + b"\xa0" * 12
        data[directory + 30] = 77
        data[:4] = bytes((0, 3, 0x58, 0x59))
        for track in range(1, 36):
            sectors = 21 if track < 18 else 19 if track < 25 else 18 if track < 31 else 17
            free = (1 << sectors) - 1
            if track == 1:
                free &= ~1
            elif track == 18:
                free &= ~3
            index = header + track * 4
            data[index:index + 4] = bytes((free.bit_count(),)) + free.to_bytes(3, "little")
        return data

    def test_map_oracle_accepts_repaired_orphan(self):
        """A rebuilt map frees an ownerless block without changing a surviving count."""
        final = self.repaired_d64()
        initial = bytearray(final)
        entry = 357 * 256 + 8
        initial[entry] -= 1
        initial[entry + 1] &= ~1
        with tempfile.TemporaryDirectory() as temporary:
            first, last = Path(temporary) / "initial.d64", Path(temporary) / "final.d64"
            first.write_bytes(initial)
            last.write_bytes(final)
            reference.check_validate_media("4040", first, last)

    def test_map_oracle_rejects_false_repair_and_corruption(self):
        """Do not accept bad counts, live-block loss, normalized counts or corrupt chains."""
        clean = self.repaired_d64()
        header, directory = 357 * 256, 358 * 256
        mutations = (
            (header + 4, clean[header + 4] + 1),
            (header + 5, clean[header + 5] | 1),
            (directory + 30, 1),
            (directory + 2, 2),
            (0, 1),
        )
        with tempfile.TemporaryDirectory() as temporary:
            first, last = Path(temporary) / "initial.d64", Path(temporary) / "final.d64"
            first.write_bytes(clean)
            for offset, value in mutations:
                candidate = bytearray(clean)
                candidate[offset] = value
                last.write_bytes(candidate)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    reference.check_validate_media("4040", first, last)


class ScratchObservationTest(unittest.TestCase):
    """Protect scratched counts, retained GET progress and the complete scratch oracle."""

    def observations(self, model):
        """Build exact statuses and data from the source-derived scenario contract."""
        messages = {1: "01, FILES SCRATCHED", 30: "30,SYNTAX ERROR", 34: "34,SYNTAX ERROR",
                    73: "73,CBM DOS V2" if model == "4040" else "73,CBM DOS V2.7"}
        result = {}
        for name, _, channel, _, expected in reference.SCRATCH_STEPS:
            if channel == 15:
                code, count = expected
                payload = (messages[code] + f",{count:02d},00"
                           + ("\r" if model == "4040" else ",0\r")).encode("ascii")
                status = [0] * (len(payload) - 1) + [64]
            else:
                payload = expected
                status = [0] * len(payload)
                if name == "scratch_one_prefix":
                    status[-1] = 64
            result[name] = {"bytes": list(payload), "kernal_st": status}
        return result

    def test_complete_models(self):
        """Retain exact count and drive fields for every selected native model."""
        for model in reference.MODELS:
            with self.subTest(model=model):
                reference.check_scratch_observations(model, self.observations(model))

    def test_incomplete_capture_fails(self):
        """Missing negative or writer cases cannot be called native parity."""
        observations = self.observations("4040")
        del observations["scratch_active_writer"]
        with self.assertRaisesRegex(ValueError, "coverage"):
            reference.check_scratch_observations("4040", observations)

    def test_wrong_count_fails(self):
        """Successful status 01 is insufficient if the scratched count differs."""
        observations = self.observations("4040")
        observations["scratch_group"] = observations["scratch_missing"]
        with self.assertRaisesRegex(ValueError, "scratch_group"):
            reference.check_scratch_observations("4040", observations)

    def test_active_file_and_cursor_failures(self):
        """Do not permit deletion of an open file or a rewound retained reader."""
        for name in ("scratch_active_reader", "scratch_active_writer", "scratch_reader_resume"):
            observations = self.observations("4040")
            observations[name]["bytes"][-2] ^= 1
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_scratch_observations("4040", observations)

    def test_eof_flags_are_not_normalized(self):
        """Require EOI on statuses/one-byte EOF, but not on bounded prefix reads."""
        for name in ("scratch_group", "scratch_one_prefix", "scratch_reader_resume"):
            observations = self.observations("8050")
            observations[name]["kernal_st"][-1] ^= 64
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                reference.check_scratch_observations("8050", observations)

    def test_program_and_capture_roundtrip(self):
        """Keep all scratch captures bounded and preserve count mailboxes."""
        capture, mailbox = bytearray(0x1c00), bytearray(32)
        mailbox[0] = reference.COMPLETE
        observations = self.observations("8250")
        program = reference.scratch_program()
        self.assertTrue(program.startswith("5 poke52,0:poke53,64:clr\n"))
        for name, (data, status, count, limit) in reference.SCRATCH_LAYOUT.items():
            sample = observations[name]
            self.assertLess(status + limit, reference.CAPTURE_ADDRESS + len(capture))
            self.assertLess(count, reference.PHASE_ADDRESS - reference.MAILBOX_ADDRESS)
            mailbox[count] = len(sample["bytes"])
            for address, key in ((data, "bytes"), (status, "kernal_st")):
                start = address - reference.CAPTURE_ADDRESS
                capture[start:start + len(sample[key])] = bytes(sample[key])
        self.assertEqual(reference.decode_capture(capture, mailbox, reference.SCRATCH_LAYOUT), observations)


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


class BuilderTest(unittest.TestCase):
    """Validate actual tracked Git changes while replacing only the expensive build."""

    def setUp(self):
        """Create a pinned fixture checkout and an eight-character-ID instrumentation patch."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.files = ("trace.cpp", "pet.cpp", "unrelated.cpp")
        for name in self.files:
            (self.source / name).write_text("original\n", encoding="ascii")
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        self.commit = self.git("rev-parse", "HEAD").stdout.strip()
        for name in self.files[:2]:
            (self.source / name).write_text("instrumented\n", encoding="ascii")
        patch_path = self.root / "drive_reference_mame.patch"
        patch_path.write_text(self.git("diff", "--abbrev=8").stdout, encoding="ascii")
        for name in self.files[:2]:
            (self.source / name).write_text("original\n", encoding="ascii")
        self.builds = 0

    def git(self, *arguments):
        """Run real Git operations against the disposable source fixture."""
        return subprocess.run(
            ["git", "-C", str(self.source), *arguments],
            check=True, capture_output=True, text=True,
        )

    def build(self):
        """Run the builder's Git checks and patch application without compiling MAME."""
        real_run = subprocess.run
        real_output = subprocess.check_output

        def run(command, **kwargs):
            """Produce a synthetic binary only after the builder reaches make."""
            if command[0] == "make":
                self.builds += 1
                (self.source / "petd").write_bytes(b"synthetic MAME")
                return subprocess.CompletedProcess(command, 0)
            return real_run(command, **kwargs)

        def check_output(command, **kwargs):
            """Retain actual Git diff behavior while supplying a compiler identity."""
            if command == ["g++", "--version"]:
                return "fixture compiler\n"
            return real_output(command, **kwargs)

        with patch.object(builder, "__file__", str(self.root / "build_drive_reference.py")), \
                patch.object(builder, "MAME_COMMIT", self.commit), \
                patch("sys.argv", ["build_drive_reference.py", "--source-dir", str(self.source)]), \
                patch.object(builder.subprocess, "run", side_effect=run), \
                patch.object(builder.subprocess, "check_output", side_effect=check_output):
            builder.main()

    def test_clean_build_and_staged_unstaged_patch_reruns(self):
        """Accept exactly the approved patch regardless of how its changes are staged."""
        self.build()
        self.build()
        self.git("add", self.files[0])
        self.build()
        self.git("add", self.files[1])
        self.git("config", "core.abbrev", "40")
        self.git("config", "diff.noprefix", "true")
        self.git("config", "diff.context", "9")
        self.build()
        self.assertEqual(self.builds, 4)
        identity = (self.source / "petd.json").read_text()
        self.assertIn(self.commit, identity)

    def test_unrelated_staged_change_rejected_before_patch_or_build(self):
        """Do not mark an instrumented build approved when tracked staged edits are hidden."""
        path = self.source / self.files[2]
        path.write_text("unrelated change\n", encoding="ascii")
        self.git("add", path.name)
        with self.assertRaisesRegex(ValueError, "source changes differ"):
            self.build()
        self.assertEqual(self.builds, 0)
        self.assertEqual((self.source / self.files[0]).read_text(), "original\n")
        self.assertEqual(path.read_text(), "unrelated change\n")

    def test_patched_checkout_rejects_extra_staged_or_unstaged_edits(self):
        """A valid instrumentation patch must not hide additional source modifications."""
        self.build()
        path = self.source / self.files[2]
        path.write_text("unrelated change\n", encoding="ascii")
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git("add", path.name)
                with self.assertRaisesRegex(ValueError, "source changes differ"):
                    self.build()
        self.assertEqual(self.builds, 1)


if __name__ == "__main__":
    unittest.main()
