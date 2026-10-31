#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Run pinned full-drive ROM cases on disposable D64/D80/D82 images.

Requires external approved ROMs, the separate 901467 GCR hardware lookup ROM,
MAME 0.285 and VICE's c1541/petcat fixture tools. A pinned,
single-cycle-interleaved source build adds composed-net IEEE traces.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from drive_wire import attoseconds, check_wire_bytes, decode_wire, label_cases, write_replay


MAME_COMMIT = "f379cd060ac3b2bd567ebbd0957aed059d6895ff"
MAME_SHA256 = "b8de871c99effd28e90c426b811694811bcb533741189f93ad63a962621ae40c"
GCR_SHA1 = "97df576397608455616331f8e837cb3404363fa2"
LOAD_ADDRESS = 0x0401
CAPTURE_ADDRESS = 0x4000
MAILBOX_ADDRESS = 0x7000
PHASE_ADDRESS = 0x701f
BASIC_MEMSIZ_LOW = 0x34
BASIC_MEMSIZ_HIGH = 0x35
COMPLETE = 255
PET_VIA_PORT_B = 0xe840
PET_PIA2_PORT_B = 0xe822
PET_ATN_MASK = 0x04
BUS_RELEASED = 0xff
TALK_DEVICE_8 = 0x48
DATA_SECONDARY = 0x62
STATUS_SECONDARY = 0x6f
SEED = bytes((0, 0x20)) + bytes(range(256)) + b"END"
WRITTEN = b"M1 ROM WRITE\r"
MODELS = {
    "4040": ("d64", ("901468-14", "901468-15", "901468-16"), "901466-04"),
    "8050": ("d80", ("901887-01", "901888-01"), "901869-01"),
    "8250": ("d82", ("901887-01", "901888-01"), "901869-01"),
}
SUFFIXES = {
    "901468-14": "uj1", "901468-15": "ul1", "901468-16": "uh1",
    "901466-04": "uk3", "901887-01": "ul1", "901888-01": "uh1",
    "901869-01": "uk3",
}
CONTROLLERS = {
    "basic-4-b000.901465-23.bin": "901465-23.ud5",
    "basic-4-c000.901465-20.bin": "901465-20.ud6",
    "basic-4-d000.901465-21.bin": "901465-21.ud7",
    "edit-4-n.901447-29.bin": "901447-29.ud8",
    "kernal-4.901465-22.bin": "901465-22.ud9",
    "characters-2.901447-10.bin": "901447-10.uf10",
}
# Each entry owns its data/status buffers and a one- or two-byte count mailbox.
LAYOUT = {
    "startup": (0x4000, 0x4040, 1, 64),
    "consumed": (0x4080, 0x40c0, 2, 64),
    "seed": (0x4200, 0x4400, 3, 512),
    "interposed": (0x4600, 0x4640, 5, 64),
    "boundary254": (0x4800, 0x4a00, 8, 512),
    "boundary255": (0x4c00, 0x4e00, 10, 512),
    "readback": (0x5000, 0x5200, 12, 512),
    "onebyte": (0x5400, 0x5600, 14, 512),
    "written_status": (0x5800, 0x5840, 16, 64),
    "reset_status": (0x5880, 0x58c0, 17, 64),
}
COMMAND_PHASE_START = 25
COMMAND_CASES = (
    "command_startup", "command_prefix", "command_u0", "command_short_u",
    "command_u0_cursor", "command_initialize", "command_closed", "command_reopen",
    "command_reset", "command_reset_closed", "command_reset_reopen", "command_unknown",
)
COMMAND_PHASES = {0: "setup", **{
    COMMAND_PHASE_START + index: name for index, name in enumerate(COMMAND_CASES)
}}
COMMAND_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x100, CAPTURE_ADDRESS + index * 0x100 + 64,
           index + 1, 64)
    for index, name in enumerate(COMMAND_CASES)
}
BASIC_READ_TIMEOUT = 0x03fc
RENAME_PHASE_START = 40
RENAME_STEPS = (
    ("rename_startup", "OPEN15,8,15", 15, 64, 73),
    ("rename_success", 'PRINT#15,"R0:RENAMED=SEED"', 15, 64, 0),
    ("rename_prefix", 'OPEN2,8,2,"0:RENAMED,P,R"', 2, 4, SEED[:4]),
    ("rename_old_missing", 'CLOSE2:PRINT#15,"R0:AGAIN=SEED"', 15, 64, 62),
    ("rename_collision", 'PRINT#15,"R0:ONE=RENAMED"', 15, 64, 63),
    ("rename_missing_precedence", 'PRINT#15,"R0:ONE=ABSENT"', 15, 64, 62),
    ("rename_self", 'PRINT#15,"R0:RENAMED=RENAMED"', 15, 64, 63),
    ("rename_pattern", 'PRINT#15,"R0:BAD*=RENAMED"', 15, 64, 30),
    ("rename_group", 'PRINT#15,"R0:BAD=RENAMED,ONE"', 15, 64, 30),
    ("rename_no_colon", 'PRINT#15,"RFOO=RENAMED"', 15, 64, 34),
    ("rename_no_equals", 'PRINT#15,"R0:BAD"', 15, 64, 30),
    ("rename_long_name", 'PRINT#15,"R0:12345678901234567=RENAMED"', 15, 64, 0),
    ("rename_restore", 'PRINT#15,"R0:RESTORED=1234567890123456"', 15, 64, 0),
    ("rename_empty_source", 'PRINT#15,"R0:BAD="', 15, 64, 62),
    ("rename_default_drive", 'PRINT#15,"R2:Q=RESTORED"', 15, 64, 0),
    ("rename_omitted_drive", 'PRINT#15,"R:RESTORED=Q"', 15, 64, 0),
    ("rename_extra_equals", 'PRINT#15,"R0:EQ=RESTORED=IGNORED"', 15, 64, 0),
    ("rename_full_keyword", 'PRINT#15,"RENAME0:RESTORED=EQ"', 15, 64, 0),
    ("rename_empty_target", 'PRINT#15,"R0:=ONE"', 15, 64, 0),
    ("rename_open_writer", 'OPEN3,8,3,"0:WRITER,S,W":PRINT#3,"RENAME WRITE";:'
     'PRINT#15,"R0:WRENAMED=WRITER"', 15, 64, 0),
    ("rename_writer_close", "CLOSE3", 15, 64, 0),
    ("rename_writer_prefix", 'OPEN2,8,2,"0:WRENAMED,S,R"', 2, 2, b"RE"),
)
RENAME_CASES = tuple(step[0] for step in RENAME_STEPS)
RENAME_PHASES = {0: "setup", **{
    RENAME_PHASE_START + index: name for index, name in enumerate(RENAME_CASES)
}}
RENAME_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x100, CAPTURE_ADDRESS + index * 0x100 + 64,
           index + 1, 64)
    for index, name in enumerate(RENAME_CASES)
}
SCRATCH_PHASE_START = 70
SCRATCH_STEPS = (
    ("scratch_startup", "OPEN15,8,15", 15, 64, (73, 0)),
    ("scratch_missing", 'PRINT#15,"S0:ABSENT"', 15, 64, (1, 0)),
    ("scratch_no_colon", 'PRINT#15,"SABSENT"', 15, 64, (34, 0)),
    ("scratch_empty", 'PRINT#15,"S0:"', 15, 64, (1, 0)),
    ("scratch_reader_prefix", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4]),
    ("scratch_active_reader", 'PRINT#15,"S0:SEED"', 15, 64, (1, 0)),
    ("scratch_reader_resume", "", 2, 2, SEED[4:6]),
    ("scratch_closed_reader", 'CLOSE2:PRINT#15,"S0:SEED"', 15, 64, (1, 1)),
    ("scratch_wrong_type", 'PRINT#15,"S0:ONE=P"', 15, 64, (1, 0)),
    ("scratch_one_prefix", 'OPEN2,8,2,"0:ONE,S,R"', 2, 4, b"A\0\2A"),
    ("scratch_right_type", 'CLOSE2:PRINT#15,"S0:ONE=S"', 15, 64, (1, 1)),
    ("scratch_group", 'PRINT#15,"S0:BOUND254,0:BOUND255"', 15, 64, (1, 2)),
    ("scratch_bad_type_group", 'PRINT#15,"S0:ABSENT=X,Y"', 15, 64, (30, 0)),
    ("scratch_bad_type_pattern", 'PRINT#15,"S0:ABSENT=*"', 15, 64, (30, 0)),
    ("scratch_active_writer", 'OPEN3,8,3,"0:WRITER,S,W":PRINT#3,"KEEP";:'
     'PRINT#15,"S0:WRITER"', 15, 64, (1, 0)),
    ("scratch_closed_writer", 'CLOSE3:PRINT#15,"S0:WRITER"', 15, 64, (1, 1)),
    ("scratch_wildcard", 'OPEN3,8,3,"0:WILD1,S,W":PRINT#3,"ONE":CLOSE3:'
     'OPEN3,8,3,"0:WILD2,S,W":PRINT#3,"TWO":CLOSE3:'
     'PRINT#15,"S0:WILD?=S"', 15, 64, (1, 2)),
    ("scratch_full_keyword", 'PRINT#15,"SCRATCH0:ABSENT"', 15, 64, (1, 0)),
    ("scratch_type_pattern", 'OPEN3,8,3,"0:TYPESEQ,S,W":PRINT#3,"SEQ":CLOSE3:'
     'OPEN3,8,3,"0:TYPEPRG,P,W":PRINT#3,"PRG":CLOSE3:'
     'PRINT#15,"S0:TYPE*=P"', 15, 64, (1, 1)),
    ("scratch_unknown_type", 'PRINT#15,"S0:TYPE*=X"', 15, 64, (1, 1)),
    ("scratch_default_group", 'PRINT#15,"S0:ABSENT,OTHER"', 15, 64, (1, 0)),
    ("scratch_survivor", 'OPEN2,8,2,"0:M1CHECK,P,R"', 2, 2, b"\x01\x04"),
)
SCRATCH_CASES = tuple(step[0] for step in SCRATCH_STEPS)
SCRATCH_PHASES = {0: "setup", **{
    SCRATCH_PHASE_START + index: name for index, name in enumerate(SCRATCH_CASES)
}}
SCRATCH_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x100, CAPTURE_ADDRESS + index * 0x100 + 64,
           index + 1, 64)
    for index, name in enumerate(SCRATCH_CASES)
}
COPY_PHASE_START = 100
COPY_STEPS = (
    ("copy_startup", "OPEN15,8,15", 15, 64, 73),
    ("copy_seed", 'PRINT#15,"C0:NEW=0:SEED"', 15, 64, 0),
    ("copy_collision", 'PRINT#15,"C0:NEW=0:ONE"', 15, 64, 63),
    ("copy_missing_precedence", 'PRINT#15,"C0:NEW=0:ABSENT"', 15, 64, 62),
    ("copy_self", 'PRINT#15,"C0:SEED=0:SEED"', 15, 64, 63),
    ("copy_one", 'PRINT#15,"C0:SINGLE=0:ONE"', 15, 64, 0),
    ("copy_boundary254", 'PRINT#15,"C0:B254=0:BOUND254"', 15, 64, 0),
    ("copy_boundary255", 'PRINT#15,"C0:B255=0:BOUND255"', 15, 64, 0),
    ("copy_concat", 'PRINT#15,"C0:JOIN=0:BOUND254,0:ONE"', 15, 64, 0),
    ("copy_append", 'PRINT#15,"C0:JOIN=0:JOIN,0:ONE"', 15, 64, 0),
    ("copy_default_append", 'PRINT#15,"C0:JOIN=JOIN,ONE"', 15, 64, 63),
    ("copy_prg_append", 'PRINT#15,"C0:SEED=0:SEED,0:ONE"', 15, 64, 64),
    ("copy_mixed", 'PRINT#15,"C0:MIXED=0:ONE,0:SEED"', 15, 64, 0),
    ("copy_four_sources", 'PRINT#15,"C0:FOUR=0:ONE,0:ONE,0:ONE,0:ONE"', 15, 64, 0),
    ("copy_pattern", 'PRINT#15,"C0:BAD=0:BO*"', 15, 64, 30),
    ("copy_target_group", 'PRINT#15,"C0:BAD,OTHER=0:ONE"', 15, 64, 30),
    ("copy_no_colon", 'PRINT#15,"CFOO=ONE"', 15, 64, 30),
    ("copy_no_equals", 'PRINT#15,"C0:BAD"', 15, 64, 30),
    ("copy_empty_source", 'PRINT#15,"C0:BAD="', 15, 64, 62),
    ("copy_empty_target", 'PRINT#15,"C0:=0:ONE"', 15, 64, 0),
    ("copy_full_keyword", 'PRINT#15,"COPY0:KEYWORD=0:ONE"', 15, 64, 0),
    ("copy_peer_prefix", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4]),
    ("copy_with_reader", 'PRINT#15,"C0:PEER=0:SEED"', 15, 64, 0),
    ("copy_peer_resume", "", 2, 2, SEED[4:6]),
)
COPY_CASES = tuple(step[0] for step in COPY_STEPS)
COPY_PHASES = {0: "setup", **{
    COPY_PHASE_START + index: name for index, name in enumerate(COPY_CASES)
}}
COPY_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x100, CAPTURE_ADDRESS + index * 0x100 + 64,
           index + 1, 64)
    for index, name in enumerate(COPY_CASES)
}
COPY_PAYLOADS = {
    "new,p": SEED, "single,s": b"A", "b254,s": bytes(range(254)),
    "b255,s": bytes(range(255)), "join,s": bytes(range(254)) + b"AA",
    "mixed,s": b"A" + SEED, "four,s": b"AAAA", "=,s": b"A",
    "keyword,s": b"A", "peer,p": SEED,
}
VALIDATE_PHASE_START = 140
VALIDATE_STEPS = (
    ("validate_startup", "OPEN15,8,15", 15, 64, 73),
    ("validate_prefix", 'OPEN2,8,2,"0:SEED,P,R"', 2, 2, SEED[:2]),
    ("validate_repair", 'PRINT#15,"V0"', 15, 64, 0),
    ("validate_closed", "", 2, 1, None),
    ("validate_writer", 'CLOSE2:OPEN3,8,3,"0:ABORT,S,W":PRINT#3,"UNFINISHED"', 15, 64, 0),
    ("validate_abandon", 'PRINT#15,"V0"', 15, 64, 0),
    ("validate_missing", 'CLOSE3:OPEN3,8,3,"0:ABORT,S,R"', 15, 64, 62),
    ("validate_repeat", 'PRINT#15,"V0"', 15, 64, 0),
    ("validate_keyword", 'PRINT#15,"VALIDATE0"', 15, 64, 0),
    ("validate_copy", 'PRINT#15,"C0:NEW=0:SEED"', 15, 64, 0),
    ("validate_final", 'PRINT#15,"V0:IGNORED"', 15, 64, 0),
    ("validate_reopen", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4]),
)
VALIDATE_CASES = tuple(step[0] for step in VALIDATE_STEPS)
VALIDATE_PHASES = {0: "setup", **{
    VALIDATE_PHASE_START + index: name for index, name in enumerate(VALIDATE_CASES)
}}
VALIDATE_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x100, CAPTURE_ADDRESS + index * 0x100 + 64,
           index + 1, 64)
    for index, name in enumerate(VALIDATE_CASES)
}
DIRECT_PHASE_START = 170
DIRECT_STEPS = (
    ("direct_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("direct_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 4, SEED[:4], False),
    ("direct_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("direct_put_wrap", 'A$="":FORI=0TO254:A$=A$+CHR$(I):NEXT:PRINT#2,A$;CHR$(255);',
     2, 1, bytes((0,)), True),
    ("direct_tail", "", 2, 253, bytes(range(1, 254)), True),
    ("direct_repeat", "", 2, 255, bytes((255,)) + bytes(range(254)), True),
    ("direct_status", "", 15, 64, 0, True),
    ("direct_explicit", 'OPEN4,8,4,"#0"', 4, 1, bytes((0,)), False),
    ("direct_occupied", 'OPEN5,8,5,"#0"', 15, 64, 70, True),
    ("direct_reserved", 'CLOSE5:OPEN5,8,5,"#12"', 15, 64, 70, True),
    ("direct_leading_zero", 'CLOSE4:CLOSE5:OPEN5,8,5,"#000"', 5, 1, bytes((0,)), False),
    ("direct_self_busy", 'OPEN6,8,2,"#11"', 15, 64, 70, True),
    ("direct_peer_retained", "", 3, 4, SEED[4:8], False),
    ("direct_close_all", 'CLOSE15:CLOSE2:OPEN15,8,15:OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
)


def direct_steps(variant="ordinary"):
    """Run the same direct-page contract on ordinary or special LOAD/SAVE secondaries."""
    if variant == "ordinary":
        return DIRECT_STEPS
    automatic, explicit = (0, 1) if variant == "load" else (1, 0)
    steps = []
    for name, setup, channel, count, expected, eoi in DIRECT_STEPS:
        setup = setup.replace(',8,2,"#', f',8,{automatic},"#').replace(',8,4,"#', f',8,{explicit},"#')
        if variant == "load" and name == "direct_tail":
            count, expected = 254, bytes((1,)) + bytes(range(1, 254))
        elif variant == "load" and name == "direct_repeat":
            expected, eoi = bytes((255, 255)) + bytes(range(253)), False
        steps.append((name, setup, channel, count, expected, eoi))
    return tuple(steps)


DIRECT_CASES = tuple(step[0] for step in DIRECT_STEPS)
DIRECT_PHASES = {0: "setup", **{
    DIRECT_PHASE_START + index: name for index, name in enumerate(DIRECT_CASES)
}}
DIRECT_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(DIRECT_CASES)
}
POINTER_PHASE_START = 190
POINTER_STEPS = (
    ("pointer_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("pointer_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("pointer_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("pointer_zero", 'A$="":FORI=0TO254:A$=A$+CHR$(I):NEXT:PRINT#2,A$;CHR$(255);:PRINT#15,"B-P:2,0"',
     2, 1, bytes((254,)), True),
    ("pointer_tail", "", 2, 3, bytes((255, 0, 1)), False),
    ("pointer_last", 'PRINT#15,"B-P:2,255"', 2, 1, bytes((253,)), True),
    ("pointer_sticky", 'PRINT#15,"B-P:2,255":PRINT#15,"B-P:2,13"', 2, 1, bytes((11,)), True),
    ("pointer_repeat", 'PRINT#15,"B-P2,000"', 2, 3, bytes((254, 255, 0)), False),
    ("pointer_decimal_wrap", 'PRINT#15,"BUFFER-P:258,1255"', 2, 1, bytes((253,)), True),
    ("pointer_four", 'PRINT#15,"B-P:2,15,4,5"', 2, 2, bytes((13, 14)), False),
    ("pointer_fifth", 'PRINT#15,"B-P:2,0,1,2,3"', 15, 64, 30, True),
    ("pointer_missing", 'PRINT#15,"B-P:9,0"', 15, 64, 70, True),
    ("pointer_peer_retained", "", 3, 4, SEED[2:6], False),
    ("pointer_retained", "", 2, 1, bytes((15,)), False),
)
POINTER_CASES = tuple(step[0] for step in POINTER_STEPS)
POINTER_PHASES = {0: "setup", **{
    POINTER_PHASE_START + index: name for index, name in enumerate(POINTER_CASES)
}}
POINTER_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(POINTER_CASES)
}
BLOCK_READ_PAGE = bytes((4, 13, 0, 255, 42)) + bytes(range(5, 255)) + bytes((99,))
BLOCK_READ_PHASE_START = 210
BLOCK_READ_STEPS = (
    ("block_read_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("block_read_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("block_read_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("block_read_counted", 'PRINT#15,"B-R:2,0,1,0"', 2, 4, BLOCK_READ_PAGE[1:5], True),
    ("block_read_repeat", "", 2, 4, BLOCK_READ_PAGE[1:5], True),
    ("block_read_fifth", 'PRINT#15,"B-R:2,0,1,0,0"', 15, 64, 30, True),
    ("block_read_raw", 'PRINT#15,"U1:2,0,1,0"', 2, 1, BLOCK_READ_PAGE[:1], False),
    ("block_read_raw_tail", "", 2, 255, BLOCK_READ_PAGE[1:], True),
    ("block_read_raw_repeat", "", 2, 4, BLOCK_READ_PAGE[1:5], False),
    ("block_read_alias", 'PRINT#15,"UA:2,0,1,0"', 2, 4, BLOCK_READ_PAGE[:4], False),
    ("block_read_invalid", 'PRINT#15,"B-R:2,0,255,255"', 15, 64, (66, 255, 255), True),
    ("block_read_missing", 'PRINT#15,"U!:9,0,1,0"', 15, 64, 70, True),
    ("block_read_peer_retained", "", 3, 4, SEED[2:6], False),
    ("block_read_retained", "", 2, 1, BLOCK_READ_PAGE[4:5], False),
)
BLOCK_READ_CASES = tuple(step[0] for step in BLOCK_READ_STEPS)
BLOCK_READ_PHASES = {0: "setup", **{
    BLOCK_READ_PHASE_START + index: name for index, name in enumerate(BLOCK_READ_CASES)
}}
BLOCK_READ_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(BLOCK_READ_CASES)
}
BLOCK_WRITE_PHASE_START = 230
BLOCK_WRITE_STEPS = (
    ("block_write_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("block_write_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("block_write_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("block_write_counted", 'PRINT#15,"U1:2,0,1,0":PRINT#15,"B-P:2,5":'
     'PRINT#15,"B-W:2,0,1,1"', 2, 4, BLOCK_READ_PAGE[1:5], False),
    ("block_write_counted_readback", 'PRINT#15,"B-R:2,0,1,1"', 2, 4, BLOCK_READ_PAGE[1:5], True),
    ("block_write_raw_state", 'PRINT#15,"B-P:2,4":PRINT#15,"B-P:2,0":'
     'PRINT#15,"U2:2,0,1,1"', 2, 1, BLOCK_READ_PAGE[:1], True),
    ("block_write_alias", 'PRINT#15,"UB:2,0,1,1"', 15, 64, 0, True),
    ("block_write_raw_readback", 'PRINT#15,"U1:2,0,1,1"', 2, 1, BLOCK_READ_PAGE[:1], False),
    ("block_write_minimum", 'PRINT#15,"B-P:2,0":PRINT#15,"B-W:2,0,1,1"',
     2, 1, BLOCK_READ_PAGE[1:2], False),
    ("block_write_minimum_readback", 'PRINT#15,"B-R:2,0,1,1"', 2, 1, BLOCK_READ_PAGE[1:2], True),
    ("block_write_invalid", 'PRINT#15,"B-W:2,0,255,255"', 15, 64, (66, 255, 255), True),
    ("block_write_missing", 'PRINT#15,"UR:9,0,1,1"', 15, 64, 70, True),
    ("block_write_peer_retained", "", 3, 4, SEED[2:6], False),
    ("block_write_maximum", 'PRINT#15,"B-P:2,255":PRINT#15,"B-W:2,0,1,1":'
     'PRINT#15,"UB:2,0,1,1"', 2, 1, BLOCK_READ_PAGE[1:2], True),
)
BLOCK_WRITE_CASES = tuple(step[0] for step in BLOCK_WRITE_STEPS)
BLOCK_WRITE_PHASES = {0: "setup", **{
    BLOCK_WRITE_PHASE_START + index: name for index, name in enumerate(BLOCK_WRITE_CASES)
}}
BLOCK_WRITE_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(BLOCK_WRITE_CASES)
}
BLOCK_MAP_PHASE_START = 10
BLOCK_MAP_STEPS = (
    ("block_map_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("block_map_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("block_map_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("block_map_free", 'PRINT#15,"B-F:0,1,0"', 15, 64, 0, True),
    ("block_map_claim", 'PRINT#15,"B-A:0,1,0"', 15, 64, 0, True),
    ("block_map_successor", 'PRINT#15,"B-A:0,1,0"', 15, 64, (65, 1, 1), True),
    ("block_map_claim_successor", 'PRINT#15,"B-A:0,1,1"', 15, 64, 0, True),
    ("block_map_release", 'PRINT#15,"B-F:0,1,0"', 15, 64, 0, True),
    ("block_map_repeat_release", 'PRINT#15,"B-F:0,1,0"', 15, 64, 0, True),
    ("block_map_reclaim", 'PRINT#15,"B-A:0,1,0"', 15, 64, 0, True),
    ("block_map_invalid", 'PRINT#15,"B-F:0,255,255"', 15, 64, (66, 255, 255), True),
    ("block_map_exhausted", 'PRINT#15,"B-A:0,{last_track},{last_sector}"',
     15, 64, (65, 0, 0), True),
    ("block_map_peer_retained", "", 3, 4, SEED[2:6], False),
    ("block_map_persist", 'CLOSE2:OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
)
BLOCK_MAP_CASES = tuple(step[0] for step in BLOCK_MAP_STEPS)
BLOCK_MAP_PHASES = {0: "setup", **{
    BLOCK_MAP_PHASE_START + index: name for index, name in enumerate(BLOCK_MAP_CASES)
}}
BLOCK_MAP_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(BLOCK_MAP_CASES)
}
WRITE_VERSION_PHASE_START = 30
WRITE_VERSION_STEPS = (
    ("write_version_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("write_version_initialize", 'PRINT#15,"I0"', 15, 64, 0, True),
    ("write_version_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("write_version_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("write_version_read", 'PRINT#15,"U1:2,0,1,0"', 15, 64, 0, True),
    ("write_version_position", 'PRINT#15,"B-P:2,5"', 15, 64, 0, True),
    ("write_version_counted", 'PRINT#15,"B-W:2,0,1,1"', 15, 64, (73, 1, 1), True),
    ("write_version_counted_state", "", 2, 4, bytes((5, 0, 255, 42)), False),
    ("write_version_raw", 'PRINT#15,"UB:2,0,1,1"', 15, 64, (73, 1, 1), True),
    ("write_version_raw_state", "", 2, 1, bytes((5,)), False),
    ("write_version_readback", 'PRINT#15,"U1:2,0,1,1"', 2, 4, bytes(4), False),
    ("write_version_peer_retained", "", 3, 4, SEED[2:6], False),
)
WRITE_VERSION_CASES = tuple(step[0] for step in WRITE_VERSION_STEPS)
WRITE_VERSION_PHASES = {0: "setup", **{
    WRITE_VERSION_PHASE_START + index: name for index, name in enumerate(WRITE_VERSION_CASES)
}}
WRITE_VERSION_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(WRITE_VERSION_CASES)
}
WARM_RESET_PHASE_START = 50
WARM_RESET_STEPS = (
    ("warm_reset_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("warm_reset_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("warm_reset_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
    ("warm_reset_fill", 'PRINT#15,"B-P:2,0":PRINT#2,"WARM";:PRINT#15,"B-A:0,1,0"',
     15, 64, 0, True),
    ("warm_reset_nmi", 'PRINT#15,"UI"', 15, 64, 73, True),
    ("warm_reset_short", 'PRINT#15,"U";', 15, 64, 73, True),
    ("warm_reset_closed", "", 3, 1, None, False),
    ("warm_reset_before_open", 'PRINT#15,"B-A:0,1,0"', 15, 64, (65, 1, 1), True),
    ("warm_reset_page", 'CLOSE2:CLOSE3:OPEN2,8,2,"#11":PRINT#15,"B-P:2,0"',
     2, 4, b"WARM", False),
    ("warm_reset_cached_map", 'PRINT#15,"B-A:0,1,0"', 15, 64, (65, 1, 1), True),
    ("warm_reset_alias", 'PRINT#15,"U9"', 15, 64, 73, True),
    ("warm_reset_alias_page", 'CLOSE2:OPEN2,8,2,"#11":PRINT#15,"B-P:2,0"',
     2, 4, b"WARM", False),
    ("warm_reset_persist", "CLOSE2", 15, 64, 0, True),
    ("warm_reset_peer_reopen", 'OPEN3,8,3,"0:SEED,P,R"', 3, 4, SEED[:4], False),
)
WARM_RESET_CASES = tuple(step[0] for step in WARM_RESET_STEPS)
WARM_RESET_PHASES = {0: "setup", **{
    WARM_RESET_PHASE_START + index: name for index, name in enumerate(WARM_RESET_CASES)
}}
WARM_RESET_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(WARM_RESET_CASES)
}
USER_INTERRUPT_PHASE_START = 70
USER_INTERRUPT_STEPS = (
    ("user_interrupt_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("user_interrupt_peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
    ("user_interrupt_number", 'OPEN2,8,2,"#11"', 2, 1, bytes((11,)), False),
    ("user_interrupt_fill", 'PRINT#15,"B-P:2,0":PRINT#2,"IRQ!";:'
     'PRINT#15,"B-A:0,1,0":PRINT#15,"B-P:2,0"', 15, 64, 0, True),
    ("user_interrupt_error", 'PRINT#15,"?"', 15, 64, 31, True),
    ("user_interrupt_vector", 'PRINT#15,"UK"', 15, 64, 0, True),
    ("user_interrupt_short", 'PRINT#15,"U";', 15, 64, 0, True),
    ("user_interrupt_page", "", 2, 4, b"IRQ!", False),
    ("user_interrupt_peer_retained", "", 3, 4, SEED[2:6], False),
    ("user_interrupt_semicolon", 'PRINT#15,"U;"', 15, 64, 0, True),
    ("user_interrupt_folded", 'PRINT#15,"U["', 15, 64, 0, True),
    ("user_interrupt_map", 'PRINT#15,"B-A:0,1,0"', 15, 64, (65, 1, 1), True),
    ("user_interrupt_persist", "CLOSE2", 15, 64, 0, True),
    ("user_interrupt_reuse", 'OPEN2,8,2,"#11":PRINT#15,"B-P:2,0"', 2, 4, b"IRQ!", False),
)
USER_INTERRUPT_CASES = tuple(step[0] for step in USER_INTERRUPT_STEPS)
USER_INTERRUPT_PHASES = {0: "setup", **{
    USER_INTERRUPT_PHASE_START + index: name for index, name in enumerate(USER_INTERRUPT_CASES)
}}
USER_INTERRUPT_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256,
           index + 1, 255)
    for index, name in enumerate(USER_INTERRUPT_CASES)
}


COPY_ALL_PHASE_START = 70
FORMAT_PHASE_START = 110


def format_listing(model, variant):
    """Construct the native header/free row independently of captured DOS bytes."""
    header = bytearray((1, 4, 1, 1, 0, 0, 0x12, ord('"')))
    label = b"M1 FIXTURE" if variant == "duplicate" else b"M4 FORMATTED"
    disk_id = b"AB" if variant == "full" else b"01"
    header.extend(label.ljust(16, b" ") + b'" ' + disk_id + b" 2")
    header.extend(b"A\0" if model == "4040" else b"C\0")
    if variant == "duplicate":
        return bytes(header)
    free = {"4040": 664, "8050": 2052, "8250": 4133}[model]
    return bytes(header) + b"\1\1" + free.to_bytes(2, "little") + b"BLOCKS FREE.".ljust(25, b" ") + b"\0\0\0"


def format_steps(model, variant):
    """Exercise complete NEW/DUPLICATE commands and native post-command listings."""
    command = "D0=1" if variant == "duplicate" else "N0:M4 FORMATTED" + (",AB" if variant == "full" else "")
    listing = format_listing(model, variant)
    steps = [
        ("format_startup", "OPEN15,8,15", 15, 64, 73, True),
        ("format_bad_syntax", 'PRINT#15,"D0=0"' if variant == "duplicate" else 'PRINT#15,"NOCOLON"',
         15, 64, 30 if variant == "duplicate" else 34, True),
        ("format_success", 'PRINT#15,"' + command + '"', 15, 64, (0, 0, 0, 0), True),
        ("format_listing", 'OPEN2,8,0,"$0"', 2, len(listing), listing, variant != "duplicate"),
        ("format_initialize", 'CLOSE2:PRINT#15,"I0"', 15, 64, (0, 0, 0, 0), True),
        ("format_reopened", 'OPEN2,8,0,"$0"', 2, len(listing), listing, variant != "duplicate"),
    ]
    if variant == "duplicate":
        steps.append(("format_copied_file", 'CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 8, SEED[:8], False))
    return tuple(steps)


FORMAT_LAYOUT = {
    name: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256, index + 1, 255)
    for index, name in enumerate(("format_startup", "format_bad_syntax", "format_success",
                                  "format_listing", "format_initialize", "format_reopened", "format_copied_file"))
}
FORMAT_PHASES = {0: "setup", **{
    FORMAT_PHASE_START + index: name for index, name in enumerate(FORMAT_LAYOUT)
}}


def format_layout(variant):
    """Include only observations actually executed by this format variant."""
    return {name: layout for name, layout in FORMAT_LAYOUT.items()
            if variant == "duplicate" or name != "format_copied_file"}


def format_expected_image(model, variant, initial):
    """Build NEW's complete expected raw image from geometry and reserved metadata."""
    result = bytearray(len(initial)) if variant == "full" else bytearray(initial)
    tracks = {"4040": 35, "8050": 77, "8250": 154}[model]
    parts = (tracks + 49) // 50
    directory = 18 if model == "4040" else 39
    version = ord("A" if model == "4040" else "C")
    header = bytearray(256)
    header[0:3] = bytes((18, 1, version)) if model == "4040" else bytes((38, 0, version))
    name = 144 if model == "4040" else 6
    header[name:name + 27] = b"\xa0" * 27
    header[name:name + 12] = b"M4 FORMATTED"
    old_header = image_offset(model, directory, 0)
    header[name + 18:name + 20] = b"AB" if variant == "full" else initial[old_header + name + 18:old_header + name + 20]
    header[name + 21:name + 23] = bytes((ord("2"), version))
    for part in range(parts):
        page = header if model == "4040" else bytearray(256)
        low, high = part * 50 + 1, min((part + 1) * 50 + 1, tracks + 1)
        if model != "4040":
            page[:6] = bytes((38 if part + 1 < parts else 39, (part + 1) * 3 if part + 1 < parts else 1,
                              version, 0, low, high))
        for track in range(low, high):
            entry = track * 4 if model == "4040" else 6 + (track - low) * 5
            for sector in range(image_sectors(model, track)):
                reserved = (track == directory and sector < 2) or (
                    model != "4040" and track == 38 and sector % 3 == 0 and sector // 3 < parts)
                if not reserved:
                    page[entry] += 1
                    page[entry + 1 + sector // 8] |= 1 << (sector % 8)
        offset = image_offset(model, directory if model == "4040" else 38, part * 3)
        result[offset:offset + 256] = page
    result[old_header:old_header + 256] = header
    offset = image_offset(model, directory, 1)
    result[offset:offset + 256] = b"\0\xff" + bytes(254)
    return bytes(result)


def check_format_media(model, variant, initial, final, source=None):
    """Reject any incorrect NEW byte, or any DUPLICATE difference from its source."""
    expected = source.read_bytes() if variant == "duplicate" else format_expected_image(
        model, variant, initial.read_bytes())
    if final.read_bytes() != expected:
        raise ValueError("format/duplicate complete persisted image mismatch")


COPY_ALL_STEPS = (
    ("copy_all_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("copy_all_peer", 'OPEN3,8,3,"1:SEED,P,R"', 3, 4, SEED[:4], False),
    ("copy_all_same", 'PRINT#15,"C0=0"', 15, 64, (30, 0, 0, 1), True),
    ("copy_all_bad_drive", 'PRINT#15,"C2=1"', 15, 64, (30, 0, 0, 1), True),
    ("copy_all_not_named", 'PRINT#15,"CFOO=ONE"', 15, 64, (30, 0, 0, 1), True),
    ("copy_all_success", 'PRINT#15,"C0=1"', 15, 64, (0, 0, 0, 1), True),
    ("copy_all_seed", 'OPEN2,8,2,"0:SEED,P,R"', 2, 8, SEED[:8], False),
    ("copy_all_one", 'CLOSE2:OPEN2,8,2,"0:ONE,S,R"', 2, 4, b"A\0\2A", True),
    ("copy_all_bound254", 'CLOSE2:OPEN2,8,2,"0:BOUND254,S,R"', 2, 254, bytes(range(254)), True),
    ("copy_all_bound255", 'CLOSE2:OPEN2,8,2,"0:BOUND255,S,R"', 2, 255, bytes(range(255)), True),
    ("copy_all_peer_retained", "", 3, 4, SEED[4:8], False),
    ("copy_all_exists", 'PRINT#15,"C0=1"', 15, 64, 63, True),
    ("copy_all_alias", 'PRINT#15,"COPY0=1IGNORED"', 15, 64, 63, True),
    ("copy_all_recovery", 'CLOSE2:PRINT#15,"C0:EXTRA=1:ONE"', 15, 64, 0, True),
)
COPY_ALL_LAYOUT = {
    step[0]: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256, index + 1, 255)
    for index, step in enumerate(COPY_ALL_STEPS)
}
COPY_ALL_PHASES = {0: "setup", **{
    COPY_ALL_PHASE_START + index: step[0] for index, step in enumerate(COPY_ALL_STEPS)
}}
COPY_PATTERN_STEPS = (
    ("copy_pattern_startup", "OPEN15,8,15", 15, 64, 73, True),
    ("copy_pattern_peer", 'OPEN3,8,3,"1:SEED,P,R"', 3, 4, SEED[:4], False),
    ("copy_pattern_bad_target", 'PRINT#15,"C0:B*=1:SEED"', 15, 64, (30, 0, 0, 1), True),
    ("copy_pattern_bad_group", 'PRINT#15,"C0:*=1:SEED,1:ONE"', 15, 64, (30, 0, 0, 1), True),
    ("copy_pattern_same", 'PRINT#15,"C1:*=1:SEED"', 15, 64, (30, 0, 0, 1), True),
    ("copy_pattern_missing", 'PRINT#15,"C0:*=1:NONE?"', 15, 64, (0, 0, 0, 1), True),
    ("copy_pattern_exact", 'PRINT#15,"C0:*IGNORED=1:ONE"', 15, 64, (0, 0, 0, 1), True),
    ("copy_pattern_one", 'OPEN2,8,2,"0:ONE,S,R"', 2, 4, b"A\0\2A", True),
    ("copy_pattern_question", 'PRINT#15,"C0:*=1:BOUND25?"', 15, 64, (0, 0, 0, 1), True),
    ("copy_pattern_bound254", 'CLOSE2:OPEN2,8,2,"0:BOUND254,S,R"', 2, 254, bytes(range(254)), True),
    ("copy_pattern_bound255", 'CLOSE2:OPEN2,8,2,"0:BOUND255,S,R"', 2, 255, bytes(range(255)), True),
    ("copy_pattern_peer_retained", "", 3, 4, SEED[4:8], False),
    ("copy_pattern_star", 'PRINT#15,"C0:*=1:S*IGNORED"', 15, 64, (0, 0, 0, 1), True),
    ("copy_pattern_recovery", 'CLOSE2:PRINT#15,"C0:EXTRA=1:ONE"', 15, 64, 0, True),
)
COPY_PATTERN_LAYOUT = {
    step[0]: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 256, index + 1, 255)
    for index, step in enumerate(COPY_PATTERN_STEPS)
}
COPY_PATTERN_PHASES = {0: "setup", **{
    COPY_ALL_PHASE_START + index: step[0] for index, step in enumerate(COPY_PATTERN_STEPS)
}}


def copy_all_steps(model, variant="all"):
    """Select whole-drive or patterned iteration, retaining the 4040 same-drive behavior."""
    if variant != "pattern":
        return COPY_ALL_STEPS
    return tuple((*step[:4], (63, 0, 0, 1), step[5])
                 if model == "4040" and step[0] == "copy_pattern_same" else step
                 for step in COPY_PATTERN_STEPS)


def copy_all_layout(variant="all"):
    """Select the bounded binary mailbox slots for this directory-copy scenario."""
    return COPY_PATTERN_LAYOUT if variant == "pattern" else COPY_ALL_LAYOUT


def copy_all_program(model="4040", variant="all"):
    """Capture directory-copy selection, file iteration, retained peers and recovery."""
    return binary_program(copy_all_steps(model, variant), copy_all_layout(variant), COPY_ALL_PHASE_START)


def check_copy_all_observations(model, observations, variant="all"):
    """Require every whole-drive copy status, binary payload and retained peer byte."""
    check_block_observations(model, observations, copy_all_steps(model, variant),
                             copy_all_layout(variant), "copy-" + variant)


def check_copy_all_source(initial, final):
    """Require the complete source disk, including metadata and unused tails, unchanged."""
    if initial.read_bytes() != final.read_bytes():
        raise ValueError("copy-all changed the complete source image")


READER_POINTER_PHASE_START = 70
READER_POINTER_STEPS = {
    "write": (
        ("reader_write_startup", "OPEN15,8,15", 15, 64, 73, True),
        ("reader_write_peer", 'OPEN3,8,3,"0:BOUND254,S,R"', 3, 2, bytes((0, 1)), False),
        ("reader_write_prefix", 'OPEN2,8,2,"SEED,P,R"', 2, 4, SEED[:4], False),
        ("reader_write_counted", 'PRINT#15,"B-W:2,0,1,0"', 2, 2, "write_head", False),
        ("reader_write_counted_status", "", 15, 64, 0, True),
        ("reader_write_raw", 'PRINT#15,"U2:2,0,1,1"', 2, 2, SEED[:2], False),
        ("reader_write_raw_status", "", 15, 64, 0, True),
        ("reader_write_cross", 'CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 253, SEED[:253], False),
        ("reader_write_active", 'PRINT#15,"UB:2,0,1,2"', 2, 2, bytes((251, 252)), False),
        ("reader_write_last", 'PRINT#15,"B-P:2,8"', 2, 1, b"D", True),
        ("reader_write_exhausted", 'PRINT#15,"UB:2,0,1,3"', 2, 1, None, False),
        ("reader_write_sticky", 'PRINT#15,"B-P:2,8":PRINT#15,"B-W:2,0,1,4"', 2, 1, bytes((8,)), True),
        ("reader_write_peer_retained", "", 3, 2, bytes((2, 3)), False),
        ("reader_write_reopened", 'CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False),
    ),
    "block": (
        ("reader_block_startup", "OPEN15,8,15", 15, 64, 73, True),
        ("reader_block_peer", 'OPEN3,8,3,"0:BOUND254,S,R"', 3, 2, bytes((0, 1)), False),
        ("reader_block_prefix", 'OPEN2,8,2,"0:SEED,P,R"', 2, 253, SEED[:253], False),
        ("reader_block_counted", 'PRINT#15,"B-R:2,0,1,0"', 2, 5,
         BLOCK_READ_PAGE[1:2] + BLOCK_READ_PAGE[1:5], True),
        ("reader_block_status", "", 15, 64, 0, True),
        ("reader_block_exhausted", "", 2, 1, None, False),
        ("reader_block_revived", 'PRINT#15,"B-P:2,4"', 2, 1, BLOCK_READ_PAGE[4:5], True),
        ("reader_block_raw", 'PRINT#15,"U1:2,0,1,0"', 2, 4,
         BLOCK_READ_PAGE[:1] + BLOCK_READ_PAGE[:3], False),
        ("reader_block_raw_tail", "", 2, 253, BLOCK_READ_PAGE[3:], True),
        ("reader_block_raw_status", "", 15, 64, 0, True),
        ("reader_block_raw_exhausted", "", 2, 1, None, False),
        ("reader_block_alias", 'PRINT#15,"UA:2,0,1,0"', 2, 4,
         BLOCK_READ_PAGE[:1] + BLOCK_READ_PAGE[:3], False),
        ("reader_block_peer_retained", "", 3, 2, bytes((2, 3)), False),
        ("reader_block_reopened", 'CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False),
    ),
    "linked": (
        ("reader_pointer_startup", "OPEN15,8,15", 15, 64, 73, True),
        ("reader_pointer_peer", 'OPEN3,8,3,"0:BOUND254,S,R"', 3, 2, bytes((0, 1)), False),
        ("reader_pointer_prefix", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False),
        ("reader_pointer_duplicate", 'PRINT#15,"B-P:2,4"', 2, 4, bytes((0, 0, 1, 2)), False),
        ("reader_pointer_cross", 'PRINT#15,"B-P:2,254"', 2, 3, bytes((250, 250, 251)), False),
        ("reader_pointer_active", 'PRINT#15,"B-P:2,2"', 2, 3, bytes((252, 252, 253)), False),
        ("reader_pointer_last", 'PRINT#15,"B-P:2,8"', 2, 1, b"D", True),
        ("reader_pointer_ok", 'PRINT#15,"B-P:2,2"', 15, 64, 0, True),
        ("reader_pointer_exhausted", "", 2, 1, None, False),
        ("reader_pointer_revived", 'PRINT#15,"B-P:2,8"', 2, 1, b"D", True),
        ("reader_pointer_reopened", 'CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 1, SEED[:1], False),
        ("reader_pointer_link", 'PRINT#15,"B-P:2,0"', 2, 1, "link", True),
        ("reader_pointer_peer_retained", "", 3, 2, bytes((2, 3)), False),
        ("reader_pointer_link_revived", 'PRINT#15,"B-P:2,0"', 2, 1, "link", True),
    ),
    "terminal": (
        ("reader_pointer_startup", "OPEN15,8,15", 15, 64, 73, True),
        ("reader_pointer_singleton", 'OPEN2,8,2,"0:ONE,S,R"', 15, 64, 0, True),
        ("reader_pointer_last", 'PRINT#15,"B-P:2,2"', 2, 1, b"A", True),
        ("reader_pointer_exhausted", 'PRINT#15,"B-P:2,0"', 2, 1, None, False),
        ("reader_pointer_revived", 'PRINT#15,"B-P:2,2"', 2, 1, b"A", True),
        ("reader_pointer_quirk", 'CLOSE2:OPEN2,8,2,"0:ONE,S,R"', 2, 1, b"A", False),
        ("reader_pointer_header", 'PRINT#15,"B-P:2,0"', 2, 4, bytes((0, 0, 2, 65)), True),
        ("reader_pointer_bound", 'CLOSE2:OPEN2,8,2,"0:BOUND254,S,R"', 2, 2, bytes((0, 1)), False),
        ("reader_pointer_before_last", 'PRINT#15,"B-P:2,254"', 2, 2, bytes((252, 252)), False),
        ("reader_pointer_sticky", 'PRINT#15,"B-P:2,2"', 2, 1, bytes((0,)), True),
        ("reader_pointer_bound_revived", 'PRINT#15,"B-P:2,255"', 2, 1, bytes((253,)), True),
        ("reader_pointer_tail_setup", 'CLOSE2:OPEN2,8,2,"0:ONE,S,R"', 15, 64, 0, True),
        ("reader_pointer_tail", 'PRINT#15,"B-P:2,255"', 2, 2, "tail", True),
        ("reader_pointer_tail_revived", 'PRINT#15,"B-P:2,2"', 2, 1, b"A", True),
    ),
}
READER_POINTER_LAYOUTS = {
    variant: {step[0]: (CAPTURE_ADDRESS + index * 0x200, CAPTURE_ADDRESS + index * 0x200 + 0x100,
                       index + 1, 255)
              for index, step in enumerate(steps)}
    for variant, steps in READER_POINTER_STEPS.items()
}
READER_POINTER_PHASES = {
    variant: {0: "setup", **{READER_POINTER_PHASE_START + index: step[0]
                           for index, step in enumerate(steps)}}
    for variant, steps in READER_POINTER_STEPS.items()
}


def reader_pointer_values(model, initial):
    """Derive link/tail expectations from immutable raw fixture entries, not DOS output."""
    data = initial.read_bytes()
    directory = image_offset(model, 18 if model == "4040" else 39, 1)
    pages = {}
    for index in range(8):
        entry = directory + index * 32
        name = data[entry + 5:entry + 21].rstrip(b"\xa0").upper()
        if data[entry + 2] & 0x80 and name in (b"SEED", b"ONE"):
            offset = image_offset(model, data[entry + 3], data[entry + 4])
            pages[name] = data[offset:offset + 256]
    if set(pages) != {b"SEED", b"ONE"} or not pages[b"SEED"][0]:
        raise ValueError("reader-pointer fixture lacks linked SEED or singleton ONE")
    return {"link": pages[b"SEED"][:1], "tail": pages[b"ONE"][-1:] * 2}


def reader_pointer_program(variant):
    """Capture ordinary-reader pointer/block edits, page changes and readiness."""
    return binary_program(READER_POINTER_STEPS[variant], READER_POINTER_LAYOUTS[variant],
                          READER_POINTER_PHASE_START)


def check_reader_pointer_observations(model, variant, observations, initial):
    """Require every variant observation with independently derived raw link/tail bytes."""
    values = reader_pointer_values(model, initial)
    if variant == "write":
        first, _ = reader_write_pages(model, initial.read_bytes())
        values["write_head"] = first[1:2] * 2
    steps = tuple((name, setup, channel, count, values.get(expected, expected), eoi)
                  for name, setup, channel, count, expected, eoi in READER_POINTER_STEPS[variant])
    check_block_observations(model, observations, steps, READER_POINTER_LAYOUTS[variant], "reader-pointer")


def check_reader_pointer_media(initial, final):
    """Reject any full-image mutation by the read-only reader-pointer sequence."""
    if initial.read_bytes() != final.read_bytes():
        raise ValueError("reader-pointer changed the complete persisted image")


def reader_write_pages(model, data):
    """Resolve and validate both immutable SEED pages independently of drive output."""
    root = image_offset(model, 18 if model == "4040" else 39, 1)
    for entry in range(root, root + 256, 32):
        if data[entry + 2] & 0x80 and data[entry + 5:entry + 21].rstrip(b"\xa0").upper() == b"SEED":
            offset = image_offset(model, data[entry + 3], data[entry + 4])
            first = data[offset:offset + 256]
            offset = image_offset(model, first[0], first[1])
            last = data[offset:offset + 256]
            if first[2:] == SEED[:254] and last[:2] == bytes((0, 8)) and last[2:9] == SEED[254:]:
                return first, last
            break
    raise ValueError("reader-write fixture lacks the independently encoded two-page SEED")


def reader_write_fixture(model, path):
    """Check all five raw write destinations are free without changing the source image."""
    data = path.read_bytes()
    reader_write_pages(model, data)
    entry = fixture_bam_entry(model, data, 1)
    if data[entry + 1] & 0x1f != 0x1f:
        raise ValueError("reader-write fixture destinations 1/0 through 1/4 must be free")


def check_reader_write_media(model, initial, final):
    """Require exact B-W counts, unchanged U2/UB pages and equality everywhere else."""
    expected = bytearray(initial.read_bytes())
    first, last = reader_write_pages(model, expected)
    counted = bytes((6,)) + first[1:]
    sticky = bytes((7,)) + last[1:]
    for sector, page in enumerate((counted, counted, last, last, sticky)):
        offset = image_offset(model, 1, sector)
        expected[offset:offset + 256] = page
    if final.read_bytes() != expected:
        raise ValueError("reader-write differs from the complete independently computed persisted image")


def warm_reset_steps(model):
    """Distinguish retained 4040 maps from DOS 2.7's post-warm AUTOI reload."""
    return tuple((name, setup, channel, count,
                  0 if model != "4040" and name == "warm_reset_cached_map" else expected, eoi)
                 for name, setup, channel, count, expected, eoi in WARM_RESET_STEPS)


def write_version_steps(version):
    """Select explicit normal/zero-version success or foreign-version rejection records."""
    replacements = {} if version == "foreign" else {
        "write_version_counted": 0, "write_version_raw": 0,
        "write_version_counted_state": BLOCK_READ_PAGE[1:5],
        "write_version_readback": BLOCK_READ_PAGE[:4],
    }
    return tuple((name, setup, channel, count, replacements.get(name, expected), eoi)
                 for name, setup, channel, count, expected, eoi in WRITE_VERSION_STEPS)


def image_sectors(model, track):
    """Return native geometry independently of the firmware image layer."""
    if not 1 <= track <= (35 if model == "4040" else 77 if model == "8050" else 154):
        raise ValueError("invalid fixture track")
    if model == "4040":
        return 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17
    track = (track - 1) % 77 + 1
    return 29 if track <= 39 else 27 if track <= 53 else 25 if track <= 64 else 23


def image_offset(model, track, sector):
    """Locate one raw sector without consulting firmware or emulator metadata."""
    if not 0 <= sector < image_sectors(model, track):
        raise ValueError("invalid fixture sector")
    return (sum(image_sectors(model, previous) for previous in range(1, track)) + sector) * 256


def validate_fixture(model, path):
    """Introduce an allocated orphan and a locked, unclosed directory entry."""
    data = bytearray(path.read_bytes())
    directory = image_offset(model, 18 if model == "4040" else 39, 1)
    entries = {bytes(data[offset + 5:offset + 21]).rstrip(b"\xa0"): offset
               for offset in range(directory, directory + 256, 32)}
    data[entries[b"BOUND255"] + 2] = 0x41
    data[entries[b"ONE"] + 2] |= 0x40
    data[entries[b"SEED"] + 30:entries[b"SEED"] + 32] = bytes((77, 0))
    # Reserve a block with no directory owner, keeping the old BAM self-consistent.
    bam = image_offset(model, 18 if model == "4040" else 38, 0)
    entry = bam + (4 if model == "4040" else 6)
    if not data[entry + 1] & 1:
        raise ValueError("fixture orphan sector is unexpectedly allocated")
    data[entry] -= 1
    data[entry + 1] &= ~1
    path.write_bytes(data)


def block_read_fixture(model, path):
    """Seed one unallocated sector without changing BAM ownership or any file."""
    data = bytearray(path.read_bytes())
    bam = image_offset(model, 18 if model == "4040" else 38, 0)
    bitmap = bam + (5 if model == "4040" else 7)
    if not data[bitmap] & 1:
        raise ValueError("block-read fixture sector 1/0 is already allocated")
    offset = image_offset(model, 1, 0)
    data[offset:offset + 256] = BLOCK_READ_PAGE
    path.write_bytes(data)


def check_validate_media(model, initial, final):
    """Independently rebuild expected maps from surviving chains and check all map bytes."""
    before, data = initial.read_bytes(), final.read_bytes()
    directory_track = 18 if model == "4040" else 39
    used = set()

    def chain(track, sector, unique=True):
        """Record one bounded chain, rejecting unexpected overlap in this clean fixture."""
        seen = set()
        while track:
            key = (track, sector)
            if key in seen or (unique and key in used):
                raise ValueError("validate fixture has a cyclic or cross-linked chain")
            seen.add(key)
            used.add(key)
            offset = image_offset(model, track, sector)
            track, sector = data[offset:offset + 2]

    chain(directory_track, 0)
    track, sector = directory_track, 1
    while track:
        offset = image_offset(model, track, sector)
        for entry in range(offset, offset + 256, 32):
            if data[entry + 2]:
                if not data[entry + 2] & 0x80:
                    raise ValueError("validate left an unclosed directory entry")
                chain(data[entry + 3], data[entry + 4])
        track, sector = data[offset:offset + 2]
    directory = image_offset(model, directory_track, 1)
    if data[directory + 30:directory + 32] != bytes((77, 0)):
        raise ValueError("validate rewrote the deliberately inflated directory block count")
    tracks = 35 if model == "4040" else 77 if model == "8050" else 154
    parts = 1 if model == "4040" else (tracks + 49) // 50
    for part in range(parts):
        offset = image_offset(model, directory_track if model == "4040" else 38, part * 3)
        expected = bytearray(before[offset:offset + 256]) if model == "4040" else bytearray(256)
        low, high = (1, 36) if model == "4040" else (part * 50 + 1, min((part + 1) * 50 + 1, tracks + 1))
        if model == "4040":
            expected[:2] = bytes((18, 1))
        else:
            expected[:6] = bytes((38 if part + 1 < parts else 39,
                                  (part + 1) * 3 if part + 1 < parts else 1, 0x43, 0, low, high))
        for track in range(low, high):
            free = [sector for sector in range(image_sectors(model, track)) if (track, sector) not in used]
            index = track * 4 if model == "4040" else 6 + (track - low) * 5
            width = 3 if model == "4040" else 4
            expected[index:index + width + 1] = bytes((len(free),)) + sum(1 << sector for sector in free).to_bytes(width, "little")
        if data[offset:offset + 256] != expected:
            raise ValueError(f"independently rebuilt validate BAM mismatch: part {part}")


def sha256(path):
    """Identify an exact input or persisted artifact."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, log, **kwargs):
    """Execute a bounded tool invocation and retain its complete diagnostic output."""
    with Path(log).open("w", encoding="utf-8") as output:
        subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                       check=True, timeout=900, **kwargs)


def verified_copy(source, destination, expected):
    """Copy an approved asset only after checking its size and SHA-256 identity."""
    source = Path(source)
    payload = source.read_bytes()
    if len(payload) != expected["size"] or sha256(source) != expected["sha256"]:
        raise ValueError(f"ROM identity mismatch: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    return {"file": source.name, "size": len(payload), "sha256": sha256(source)}


def prepare_roms(args, output, manifest):
    """Stage approved external inputs under the pinned emulator's ROM filenames."""
    entries = {entry["part"]: entry for entry in manifest["drive_roms"]}
    _, main, controller = MODELS[args.model]
    identities = []
    for part in (*main, controller):
        identities.append(verified_copy(
            args.drive_rom_dir / (part + ".bin"),
            output / "roms" / ("c" + args.model) / (part + "." + SUFFIXES[part]),
            entries[part],
        ))
    controller_entries = {entry["file"]: entry for entry in manifest["controller_roms"]}
    for source, destination in CONTROLLERS.items():
        path = args.controller_rom_dir / source
        entry = controller_entries.get(source)
        if entry is None:
            # MAME's non-CRTC editor is an extra, explicitly pinned controller asset.
            entry = {"size": 2048,
                     "sha256": "44c79cce9be64be26000821957ef17ca2d86ab5cd66c1e6c4342b91fd5f04b67"}
        identities.append(verified_copy(path, output / "roms/cbm4032" / destination, entry))
    gcr = args.gcr_rom.read_bytes()
    if len(gcr) != 2048 or hashlib.sha1(gcr).hexdigest() != GCR_SHA1:
        raise ValueError("901467 hardware GCR lookup ROM identity mismatch")
    device = "c2040_fdc" if args.model == "4040" else "c8050fdc"
    target = output / "roms" / device / "901467.uk6"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(gcr)
    identities.append({"file": "901467", "size": len(gcr), "sha256": sha256(target)})
    return identities


def basic_program():
    """Build a bounded native PET controller covering status, EOF, TALK reuse and writes."""
    # Reserve capture/mailbox RAM above BASIC's heap (pinned PET V4R bassrc:118-124).
    lines = [
        f"5 POKE{BASIC_MEMSIZ_LOW},0:POKE{BASIC_MEMSIZ_HIGH},{CAPTURE_ADDRESS >> 8}:CLR",
        f"10 POKE{MAILBOX_ADDRESS},1:OPEN15,8,15",
        "20 B=16384:M=28673:P=1:GOSUB9000",
        "30 B=16512:M=28674:P=3:GOSUB9000",
        '40 B=16896:M=28675:P=5:F$="0:SEED,P,R":K=1:GOSUB9100',
        '50 B=21504:M=28686:P=11:F$="0:ONE,S,R":K=0:GOSUB9100',
        '60 B=18432:M=28680:P=13:F$="0:BOUND254,S,R":GOSUB9100',
        '70 B=19456:M=28682:P=15:F$="0:BOUND255,S,R":GOSUB9100',
        f'80 POKE{PHASE_ADDRESS},17:OPEN3,8,3,"0:RESULT,S,W"',
        '90 PRINT#3,"M1 ROM WRITE":CLOSE3',
        f"100 POKE{PHASE_ADDRESS},18:B=22528:M=28688:P=19:GOSUB9000",
        '110 B=20480:M=28684:P=21:F$="0:RESULT,S,R":GOSUB9100',
        '120 CLOSE15:OPEN15,8,15,"UJ"',
        "130 B=22656:M=28689:P=23:GOSUB9000:CLOSE15",
        f"140 POKE{MAILBOX_ADDRESS},{COMPLETE}:END",
        f"9000 POKE{PHASE_ADDRESS},P:J=0",
        "9010 GET#15,A$:V=ASC(A$+CHR$(0)):Z=ST:POKEB+J,V:POKEB+64+J,Z:J=J+1",
        "9020 IFV<>13ANDZ=0ANDJ<64THEN9010",
        f"9030 POKEM,J:POKE{PHASE_ADDRESS},P+1:RETURN",
        f"9100 POKE{PHASE_ADDRESS},P:OPEN2,8,2,F$:I=0",
        "9110 GET#2,A$:V=ASC(A$+CHR$(0)):T=ST:POKEB+I,V:POKEB+512+I,T:I=I+1",
        "9120 IFK=1ANDI=2THENGOSUB9200",
        "9130 IFT=0ANDI<512THEN9110",
        f"9140 POKEM,IAND255:POKEM+1,INT(I/256):CLOSE2:POKE{PHASE_ADDRESS},P+1:RETURN",
        "9200 BS=B:MS=M:PS=P:B=17920:M=28677:P=9:GOSUB9000:B=BS:M=MS:P=PS:RETURN",
    ]
    return "\n".join(lines).lower() + "\n"


def command_program():
    """Capture initialize/reset effects and U0's retained channel/command bytes."""
    steps = (
        ('OPEN15,8,15', 15, 64),
        ('OPEN2,8,2,"0:SEED,P,R"', 2, 2),
        ('PRINT#15,"U0"', 15, 64),
        ('PRINT#15,"U";', 15, 64),
        ("", 2, 2),
        ('PRINT#15,"I0"', 15, 64),
        ("", 2, 1),
        ('CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 2),
        ('PRINT#15,"UJ"', 15, 64),
        ("", 2, 1),
        ('CLOSE2:OPEN2,8,2,"0:SEED,P,R"', 2, 2),
        ('PRINT#15,"?"', 15, 64),
    )
    return observation_program(steps, COMMAND_CASES, COMMAND_PHASE_START, COMMAND_LAYOUT)


def observation_program(steps, names, first_phase, layout, binary=False):
    """Generate bounded BASIC GET captures with a protected heap and timeout mailbox."""
    lines = [
        f"5 POKE{BASIC_MEMSIZ_LOW},0:POKE{BASIC_MEMSIZ_HIGH},{CAPTURE_ADDRESS >> 8}:CLR",
        f"10 BO=PEEK({BASIC_READ_TIMEOUT}):POKE{BASIC_READ_TIMEOUT},0:POKE{MAILBOX_ADDRESS},1",
    ]
    for index, (name, (setup, channel, count)) in enumerate(zip(names, steps)):
        data, _, mailbox, _ = layout[name]
        prefix = setup + ":" if setup else ""
        lines.append(
            f"{20 + index * 10} POKE{PHASE_ADDRESS},{first_phase + index}:"
            f"{prefix}B={data}:M={MAILBOX_ADDRESS + mailbox}:F={channel}:N={count}:GOSUB9000")
    lines.extend((
        f"500 CLOSE2:CLOSE15:POKE{BASIC_READ_TIMEOUT},BO:POKE{MAILBOX_ADDRESS},{COMPLETE}:END",
        "9000 J=0",
        "9010 GET#F,A$:V=ASC(A$+CHR$(0)):Z=ST:POKEB+J,V:POKEB+64+J,Z:J=J+1",
        "9020 IF" + ("(F<>15ORV<>13)" if binary else "V<>13") + "ANDZ=0ANDJ<NTHEN9010",
        "9030 POKEM,J:RETURN",
    ))
    return "\n".join(lines).lower() + "\n"


def rename_program():
    """Exercise native rename parsing, error precedence and still-open writers."""
    return observation_program(
        [step[1:4] for step in RENAME_STEPS], RENAME_CASES, RENAME_PHASE_START, RENAME_LAYOUT)


def scratch_program():
    """Capture native deletion, counts, type suffixes and active-file protection."""
    return observation_program(
        [step[1:4] for step in SCRATCH_STEPS], SCRATCH_CASES, SCRATCH_PHASE_START, SCRATCH_LAYOUT)


def copy_program():
    """Capture named copy, concatenation and native same-first-source append."""
    return observation_program(
        [step[1:4] for step in COPY_STEPS], COPY_CASES, COPY_PHASE_START, COPY_LAYOUT)


def validate_program():
    """Capture map repair, writer abandonment and repeated validate commands."""
    return observation_program(
        [step[1:4] for step in VALIDATE_STEPS], VALIDATE_CASES, VALIDATE_PHASE_START, VALIDATE_LAYOUT)


def block_write_fixture(model, path):
    """Reserve no blocks, but seed a source and zero a distinct free destination."""
    data = bytearray(path.read_bytes())
    bam = image_offset(model, 18 if model == "4040" else 38, 0)
    bitmap = bam + (5 if model == "4040" else 7)
    if data[bitmap] & 3 != 3:
        raise ValueError("block-write fixture sectors 1/0 and 1/1 must be free")
    block_read_fixture(model, path)
    data = bytearray(path.read_bytes())
    offset = image_offset(model, 1, 1)
    data[offset:offset + 256] = bytes(256)
    path.write_bytes(data)


def check_block_write_media(model, initial, final):
    """Compare every image byte, including the full unallocated destination sector."""
    expected = bytearray(initial.read_bytes())
    offset = image_offset(model, 1, 1)
    page = bytearray(BLOCK_READ_PAGE)
    page[0] = 254
    expected[offset:offset + 256] = page
    if final.read_bytes() != expected:
        raise ValueError("block-write image differs outside the exact persisted raw page")


def fixture_bam_entry(model, data, track):
    """Resolve native BAM links and declared ranges independently of production code."""
    offset = image_offset(model, 18 if model == "4040" else 38, 0)
    if model == "4040":
        return offset + track * 4
    for _ in range(4):
        low, high = data[offset + 4:offset + 6]
        if low <= track < high:
            return offset + 6 + (track - low) * 5
        offset = image_offset(model, data[offset], data[offset + 1])
    raise ValueError("fixture BAM has no requested track range")


def block_map_fixture(model, path):
    """Keep initial claims free and reserve the last sector to demonstrate no-wrap exhaustion."""
    data = bytearray(path.read_bytes())
    entry = fixture_bam_entry(model, data, 1)
    if data[entry + 1] & 3 != 3:
        raise ValueError("block-map fixture sectors 1/0 and 1/1 must be free")
    track = {"4040": 35, "8050": 77, "8250": 154}[model]
    sector = image_sectors(model, track) - 1
    entry = fixture_bam_entry(model, data, track)
    index, mask = entry + 1 + sector // 8, 1 << (sector % 8)
    if data[index] & mask:
        data[index] &= ~mask
        data[entry] -= 1
    path.write_bytes(data)


def check_block_map_media(model, initial, final):
    """Require only the two final claims, with all other image bytes unchanged."""
    expected = bytearray(initial.read_bytes())
    entry = fixture_bam_entry(model, expected, 1)
    expected[entry] -= 2
    expected[entry + 1] &= ~3
    if final.read_bytes() != expected:
        raise ValueError("block-map image differs outside the exact count/bitmap claims")


def write_version_fixture(model, version, path):
    """Set native BAM versions while making the DOS 2.7 header deliberately disagree."""
    block_write_fixture(model, path)
    data = bytearray(path.read_bytes())
    header = image_offset(model, 18 if model == "4040" else 39, 0)
    value = {"foreign": 255, "zero": 0, "native": ord("A" if model == "4040" else "C")}[version]
    data[header + 2] = value if model == "4040" else 255
    if model != "4040":
        for part in range(2 if model == "8050" else 4):
            data[image_offset(model, 38, part * 3) + 2] = value
    path.write_bytes(data)


def check_write_version_media(model, version, initial, final):
    """Require a wholly unchanged rejected image or exactly one successful copied page."""
    expected = bytearray(initial.read_bytes())
    if version != "foreign":
        expected[256:512] = expected[:256]
    if final.read_bytes() != expected:
        raise ValueError("write-version image differs from the exact accepted/rejected page")


def check_warm_reset_media(model, initial, final):
    """Require the retained 4040 claim or an unchanged post-AUTOI DOS 2.7 image."""
    expected = bytearray(initial.read_bytes())
    entry = fixture_bam_entry(model, expected, 1)
    if not expected[entry + 1] & 1:
        raise ValueError("warm-reset fixture sector 1/0 must be free")
    if model == "4040":
        expected[entry] -= 1
        expected[entry + 1] &= ~1
    if final.read_bytes() != expected:
        raise ValueError("warm-reset image differs outside the retained BAM claim")


def check_user_interrupt_media(model, initial, final):
    """Require the retained dirty claim, and no other persisted image changes."""
    expected = bytearray(initial.read_bytes())
    entry = fixture_bam_entry(model, expected, 1)
    if not expected[entry + 1] & 1:
        raise ValueError("user-interrupt fixture sector 1/0 must be free")
    expected[entry] -= 1
    expected[entry + 1] &= ~1
    if final.read_bytes() != expected:
        raise ValueError("user-interrupt image differs outside the retained BAM claim")


def binary_program(steps, layout, phase_start):
    """Capture binary channel scenarios without treating an embedded CR as file EOF."""
    lines = [
        f"5 POKE{BASIC_MEMSIZ_LOW},0:POKE{BASIC_MEMSIZ_HIGH},{CAPTURE_ADDRESS >> 8}:CLR",
        f"10 BO=PEEK({BASIC_READ_TIMEOUT}):POKE{BASIC_READ_TIMEOUT},0:POKE{MAILBOX_ADDRESS},1",
    ]
    for index, (name, setup, channel, count, _, _) in enumerate(steps):
        data, _, mailbox, _ = layout[name]
        prefix = setup + ":" if setup else ""
        lines.append(
            f"{20 + index * 10} POKE{PHASE_ADDRESS},{phase_start + index}:"
            f"{prefix}B={data}:M={MAILBOX_ADDRESS + mailbox}:F={channel}:N={count}:GOSUB9000")
    lines.extend((
        f"500 CLOSE2:CLOSE15:POKE{BASIC_READ_TIMEOUT},BO:POKE{MAILBOX_ADDRESS},{COMPLETE}:END",
        "9000 J=0",
        "9010 GET#F,A$:V=ASC(A$+CHR$(0)):Z=ST:POKEB+J,V:POKEB+256+J,Z:J=J+1",
        "9020 IF(F<>15ORV<>13)ANDZ=0ANDJ<NTHEN9010",
        "9030 POKEM,J:RETURN",
    ))
    return "\n".join(lines).lower() + "\n"


def direct_program(variant="ordinary"):
    """Capture page allocation, circular GET and sticky PUT EOI."""
    return binary_program(direct_steps(variant), DIRECT_LAYOUT, DIRECT_PHASE_START)


def pointer_program():
    """Capture raw B-P offsets, sticky EOI, parser errors and retained peers."""
    return binary_program(POINTER_STEPS, POINTER_LAYOUT, POINTER_PHASE_START)


def block_read_program():
    """Capture counted versus raw block reads and retained binary channel state."""
    return binary_program(BLOCK_READ_STEPS, BLOCK_READ_LAYOUT, BLOCK_READ_PHASE_START)


def block_write_program():
    """Capture count mutation, raw state preservation and persisted direct block writes."""
    return binary_program(BLOCK_WRITE_STEPS, BLOCK_WRITE_LAYOUT, BLOCK_WRITE_PHASE_START)


def block_map_program(model):
    """Capture successor suggestions, idempotent frees and close-triggered BAM persistence."""
    track = {"4040": 35, "8050": 77, "8250": 154}[model]
    steps = [(name, setup.format(last_track=track, last_sector=image_sectors(model, track) - 1),
              channel, count, expected, eoi)
             for name, setup, channel, count, expected, eoi in BLOCK_MAP_STEPS]
    return binary_program(steps, BLOCK_MAP_LAYOUT, BLOCK_MAP_PHASE_START)


def write_version_program(version):
    """Capture cached-BAM rejection, pre-job mutations and readable foreign media."""
    return binary_program(write_version_steps(version), WRITE_VERSION_LAYOUT, WRITE_VERSION_PHASE_START)


def warm_reset_program():
    """Capture NMI aliases, stale selector, released channels and retained page/map bytes."""
    return binary_program(WARM_RESET_STEPS, WARM_RESET_LAYOUT, WARM_RESET_PHASE_START)


def user_interrupt_program():
    """Capture default IRQ aliases without resetting channels, cursors or dirty maps."""
    return binary_program(USER_INTERRUPT_STEPS, USER_INTERRUPT_LAYOUT, USER_INTERRUPT_PHASE_START)


def prepare_image(args, output):
    """Create an independent synthetic fixture and preserve its pre-execution bytes."""
    extension = MODELS[args.model][0]
    image = output / ("initial." + extension)
    # c1541 maps host lowercase to the literal uppercase PETSCII DUPLICATE header oracle.
    label = "m1 fixture,01" if args.format_check == "duplicate" else "M1 FIXTURE,01"
    run([str(args.c1541), "-format", label, extension, str(image)],
        output / "format.log")
    fixtures = {"seed.prg": SEED, "one.seq": b"A",
                "bound254.seq": bytes(range(254)), "bound255.seq": bytes(range(255))}
    for name, payload in fixtures.items():
        path = output / name
        path.write_bytes(payload)
        file_type = ",s" if name.endswith(".seq") else ",p"
        run([str(args.c1541), "-attach", str(image), "-write", str(path),
             path.stem + file_type], output / (name + ".log"))
    # Put the same read-only executable in both the reference and operator images.
    board_name = "m3check" if args.ordinary_check else "drive-check"
    board_source = Path(__file__).resolve().parents[2] / "prgs" / board_name / (board_name + ".bas")
    board_program = output / (board_name + ".prg")
    run([str(args.petcat), "-h", "-w1p", "-l", f"{LOAD_ADDRESS:04x}",
         "-o", str(board_program), "--", str(board_source)], output / "board-petcat.log")
    run([str(args.c1541), "-attach", str(image), "-write", str(board_program), "m1check,p"],
        output / "board-fixture.log")
    source = output / "controller.txt"
    edge = getattr(args, "edge_check", None)
    if edge:
        edge_fixture(args.model, edge, image)
    if args.write_version_check:
        write_version_fixture(args.model, args.write_version_check, image)
    if args.block_map_check:
        block_map_fixture(args.model, image)
    if args.block_write_check:
        block_write_fixture(args.model, image)
    if args.block_read_check or args.reader_pointer_check == "block":
        block_read_fixture(args.model, image)
    if args.reader_pointer_check == "write":
        reader_write_fixture(args.model, image)
    if args.validate_check:
        validate_fixture(args.model, image)
    program_text = (edge_program(args.model, edge) if edge else
                    binary_program(format_steps(args.model, args.format_check), format_layout(args.format_check),
                                   FORMAT_PHASE_START) if args.format_check else
                    copy_all_program(args.model, args.copy_all_check) if args.copy_all_check else reader_pointer_program(args.reader_pointer_check) if args.reader_pointer_check else user_interrupt_program() if args.user_interrupt_check else warm_reset_program() if args.warm_reset_check else write_version_program(args.write_version_check) if args.write_version_check else block_map_program(args.model) if args.block_map_check else block_write_program() if args.block_write_check else block_read_program() if args.block_read_check else pointer_program() if args.pointer_check else direct_program(args.direct_check) if args.direct_check else validate_program() if args.validate_check else copy_program() if args.copy_check else scratch_program() if args.scratch_check else rename_program() if args.rename_check
                    else command_program() if args.command_check else basic_program())
    source.write_text(program_text, encoding="ascii")
    program = output / "controller.prg"
    run([str(args.petcat), "-h", "-w1p", "-l", f"{LOAD_ADDRESS:04x}", "-o", str(program),
         "--", str(source)], output / "petcat.log")
    tokenized = program.read_bytes()
    if (len(tokenized) < 7 or int.from_bytes(tokenized[:2], "little") != LOAD_ADDRESS
            or tokenized[6] != 0x97):
        raise ValueError("PET BASIC controller did not tokenize correctly")
    if args.copy_all_check or args.format_check == "duplicate" or edge == "cache-drives":
        # Preserve drive 1 separately so both complete images remain independently checked.
        shutil.copyfile(image, output / ("source-initial." + extension))
        if edge == "cache-drives":
            edge_fixture(args.model, "cache-errors", output / ("source-initial." + extension))
        shutil.copyfile(output / ("source-initial." + extension), output / ("source-final." + extension))
        if edge != "cache-drives":
            image.unlink()
            run([str(args.c1541), "-format", "M4 TARGET,02", extension, str(image)],
                output / "target-format.log")
    mounted = output / ("final." + extension)
    shutil.copyfile(image, mounted)
    return image, mounted, program


def decode_capture(capture, mailbox, layout=LAYOUT):
    """Decode only completed, bounded per-case bytes and KERNAL status samples."""
    if len(capture) != 0x1c00 or len(mailbox) != 32 or mailbox[0] != COMPLETE:
        raise ValueError("reference controller did not complete its bounded cases")
    observations = {}
    for name, (data, status, count, limit) in layout.items():
        size = mailbox[count]
        if limit > 255:
            size += mailbox[count + 1] << 8
        if not 0 < size <= limit:
            raise ValueError(f"incomplete or oversized reference observation: {name}")
        start = data - CAPTURE_ADDRESS
        state = status - CAPTURE_ADDRESS
        observations[name] = {
            "bytes": list(capture[start:start + size]),
            "kernal_st": list(capture[state:state + size]),
        }
    return observations


def check_observations(model, observations):
    """Require native fixture results without treating current firmware as the oracle."""
    expected = {
        "seed": SEED, "onebyte": b"A\0\2A", "boundary254": bytes(range(254)),
        "boundary255": bytes(range(255)), "readback": WRITTEN,
    }
    for name, payload in expected.items():
        observation = observations[name]
        if bytes(observation["bytes"]) != payload:
            raise ValueError(f"native byte mismatch: {model} {name}")
    version = b"2" if model == "4040" else b"2.7"
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    startup = b"73,CBM DOS V" + version + trailer
    for name in ("startup", "reset_status"):
        if bytes(observations[name]["bytes"]) != startup:
            raise ValueError(f"native reset/status mismatch: {model} {name}")
    for name in ("consumed", "interposed", "written_status"):
        if bytes(observations[name]["bytes"]) != b"00, OK" + trailer:
            raise ValueError(f"native consumed/write status mismatch: {model} {name}")
    for name, observation in observations.items():
        if observation["kernal_st"] != [0] * (len(observation["bytes"]) - 1) + [64]:
            raise ValueError(f"native EOF/error status mismatch: {model} {name}")


def check_command_observations(model, observations):
    """Require ROM-derived bytes, exact status and real timeouts after channel teardown."""
    if set(observations) != set(COMMAND_CASES):
        raise ValueError("incomplete native command observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    startup = b"73,CBM DOS V" + (b"2" if model == "4040" else b"2.7") + trailer
    for name, observed in observations.items():
        if name in ("command_closed", "command_reset_closed"):
            expected, status = b"\r", [2]
        elif name in ("command_startup", "command_reset"):
            expected = startup
            status = [0] * (len(expected) - 1) + [64]
        elif name in ("command_prefix", "command_reopen", "command_reset_reopen"):
            expected, status = SEED[:2], [0, 0]
        elif name == "command_u0_cursor":
            expected, status = SEED[2:4], [0, 0]
        else:
            expected = (b"31,SYNTAX ERROR" if name == "command_unknown" else b"00, OK") + trailer
            status = [0] * (len(expected) - 1) + [64]
        if bytes(observed["bytes"]) != expected or observed["kernal_st"] != status:
            raise ValueError(f"native command observation mismatch: {model} {name}: {observed}")


def check_rename_observations(model, observations):
    """Require native rename statuses and unchanged payload prefixes, without normalization."""
    if set(observations) != set(RENAME_CASES):
        raise ValueError("incomplete native rename observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    messages = {0: b" OK", 30: b"SYNTAX ERROR", 34: b"SYNTAX ERROR",
                62: b" FILE NOT FOUND", 63: b" FILE EXISTS",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected in RENAME_STEPS:
        if channel == 15:
            payload = f"{expected:02d},".encode("ascii") + messages[expected] + trailer
            status = [0] * (len(payload) - 1) + [64]
        else:
            payload, status = expected, [0] * len(expected)
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native rename observation mismatch: {model} {name}: {observations[name]}")


def check_scratch_observations(model, observations):
    """Require exact native scratched counts, type filtering, data and EOF flags."""
    if set(observations) != set(SCRATCH_CASES):
        raise ValueError("incomplete native scratch observation coverage")
    messages = {1: b" FILES SCRATCHED", 30: b"SYNTAX ERROR", 34: b"SYNTAX ERROR",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected in SCRATCH_STEPS:
        if channel == 15:
            code, count = expected
            payload = (f"{code:02d},".encode("ascii") + messages[code]
                       + f",{count:02d},00".encode("ascii")
                       + (b"\r" if model == "4040" else b",0\r"))
            status = [0] * (len(payload) - 1) + [64]
        else:
            payload, status = expected, [0] * len(expected)
            if name == "scratch_one_prefix":
                status[-1] = 64
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native scratch observation mismatch: {model} {name}: {observations[name]}")


def check_copy_observations(model, observations):
    """Reject missing observations or differences in native copy status and peer cursors."""
    if set(observations) != set(COPY_CASES):
        raise ValueError("incomplete native copy observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    messages = {0: b" OK", 30: b"SYNTAX ERROR", 62: b" FILE NOT FOUND",
                63: b" FILE EXISTS", 64: b" FILE TYPE MISMATCH",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected in COPY_STEPS:
        if channel == 15:
            payload = f"{expected:02d},".encode("ascii") + messages[expected] + trailer
            status = [0] * (len(payload) - 1) + [64]
        else:
            payload, status = expected, [0] * len(expected)
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native copy observation mismatch: {model} {name}: {observations[name]}")


def check_validate_observations(model, observations):
    """Require exact validate statuses, discarded-reader timeout and reopened bytes."""
    if set(observations) != set(VALIDATE_CASES):
        raise ValueError("incomplete native validate observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    messages = {0: b" OK", 62: b" FILE NOT FOUND",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected in VALIDATE_STEPS:
        if channel == 15:
            payload = f"{expected:02d},".encode("ascii") + messages[expected] + trailer
            status = [0] * (len(payload) - 1) + [64]
        elif expected is None:
            payload, status = b"\r", [2]
        else:
            payload, status = expected, [0] * len(expected)
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native validate observation mismatch: {model} {name}: {observations[name]}")


def check_direct_observations(model, observations, variant="ordinary"):
    """Require binary page numbers, circular data, sticky PUT EOI and retained peers."""
    if set(observations) != set(DIRECT_CASES):
        raise ValueError("incomplete native direct observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    messages = {0: b" OK", 70: b"NO CHANNEL",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected, eoi in direct_steps(variant):
        payload = (f"{expected:02d},".encode("ascii") + messages[expected] + trailer
                   if channel == 15 else expected)
        status = [0] * len(payload)
        if eoi:
            status[-1] = 64
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native direct observation mismatch: {model} {name}: {observations[name]}")


def check_pointer_observations(model, observations):
    """Require exact B-P data, native parser errors, sticky EOI and peer continuation."""
    if set(observations) != set(POINTER_CASES):
        raise ValueError("incomplete native pointer observation coverage")
    trailer = b",00,00\r" if model == "4040" else b",00,00,0\r"
    messages = {30: b"SYNTAX ERROR", 70: b"NO CHANNEL",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected, eoi in POINTER_STEPS:
        payload = (f"{expected:02d},".encode("ascii") + messages[expected] + trailer
                   if channel == 15 else expected)
        status = [0] * len(payload)
        if eoi:
            status[-1] = 64
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native pointer observation mismatch: {model} {name}: {observations[name]}")


def check_block_read_observations(model, observations):
    """Require native count/raw boundaries, aliases, geometry errors and exact peer bytes."""
    check_block_observations(model, observations, BLOCK_READ_STEPS, BLOCK_READ_CASES, "block-read")


def check_block_write_observations(model, observations):
    """Require native count mutation, raw channel preservation and retained peer bytes."""
    check_block_observations(model, observations, BLOCK_WRITE_STEPS, BLOCK_WRITE_CASES, "block-write")


def check_block_map_observations(model, observations):
    """Require exact allocation suggestions, exhaustion and retained channel observations."""
    check_block_observations(model, observations, BLOCK_MAP_STEPS, BLOCK_MAP_CASES, "block-map")


def check_write_version_observations(model, version, observations):
    """Require exact 73 job coordinates, pre-job state and untouched foreign-media reads."""
    check_block_observations(model, observations, write_version_steps(version), WRITE_VERSION_CASES, "write-version")


def check_warm_reset_observations(model, observations):
    """Require exact warm statuses, timeout on released peers and retained RAM/cache data."""
    check_block_observations(model, observations, warm_reset_steps(model), WARM_RESET_CASES, "warm-reset")


def check_user_interrupt_observations(model, observations):
    """Require IRQ success, stale selector and unchanged prepared page/peer bytes."""
    check_block_observations(model, observations, USER_INTERRUPT_STEPS, USER_INTERRUPT_CASES, "user-interrupt")


def check_block_observations(model, observations, steps, cases, label):
    """Compare exact block-command binary records and model-specific status fields."""
    if set(observations) != set(cases):
        raise ValueError(f"incomplete native {label} observation coverage")
    messages = {0: b" OK", 1: b" FILES SCRATCHED", 30: b"SYNTAX ERROR", 31: b"SYNTAX ERROR", 32: b"SYNTAX ERROR", 34: b"SYNTAX ERROR", 62: b" FILE NOT FOUND", 63: b" FILE EXISTS", 65: b"NO BLOCK", 66: b"ILLEGAL TRACK OR SECTOR", 70: b"NO CHANNEL",
                73: b"CBM DOS V" + (b"2" if model == "4040" else b"2.7")}
    for name, _, channel, _, expected, eoi in steps:
        if channel == 15:
            code, track, sector = expected[:3] if isinstance(expected, tuple) else (expected, 0, 0)
            drive = expected[3] if isinstance(expected, tuple) and len(expected) == 4 else 0
            if model == "4040":
                track, sector = track % 100, sector % 100
            payload = (f"{code:02d},".encode("ascii") + messages[code]
                       + f",{track:02d},{sector:02d}".encode("ascii")
                       + (b"\r" if model == "4040" else f",{drive}\r".encode("ascii")))
        elif expected is None:
            payload = b"\r"
        else:
            payload = expected
        status = [2] if expected is None else [0] * len(payload)
        if eoi:
            status[-1] = 64
        if bytes(observations[name]["bytes"]) != payload or observations[name]["kernal_st"] != status:
            raise ValueError(f"native {label} observation mismatch: {model} {name}: {observations[name]}")


def controller_commands(path):
    """Project PET ATN-tagged DIO writes, not unobserved IEEE acceptance edges."""
    atn = None
    commands = []
    with Path(path).open(encoding="ascii") as stream:
        for row in csv.DictReader(stream):
            if row["direction"] != "write":
                continue
            address, value = int(row["address"], 16), int(row["value"], 16)
            if address == PET_VIA_PORT_B:
                atn = not bool(value & PET_ATN_MASK)
            elif address == PET_PIA2_PORT_B and atn and value != BUS_RELEASED:
                commands.append({"seconds": float(row["seconds"]), "byte": value ^ BUS_RELEASED})
    return commands


def check_interruption(commands, phases):
    """Require a status TALK between source-file TALK selections on the still-open channel."""
    starts = [float(row["seconds"]) for row in phases if row["phase"] == "9"]
    ends = [float(row["seconds"]) for row in phases if row["phase"] == "10"]
    seed_starts = [float(row["seconds"]) for row in phases if row["phase"] == "5"]
    seed_ends = [float(row["seconds"]) for row in phases if row["phase"] == "6"]
    if (any(len(values) != 1 for values in (starts, ends, seed_starts, seed_ends))
            or not seed_starts[0] < starts[0] < ends[0] < seed_ends[0]):
        raise ValueError("incomplete interposed-status phase coverage")
    selections = []
    for first, second in zip(commands, commands[1:]):
        if first["byte"] == TALK_DEVICE_8:
            selections.append(second)
    before = [row for row in selections if seed_starts[0] < row["seconds"] < starts[0]]
    during = [row for row in selections if starts[0] <= row["seconds"] <= ends[0]]
    after = [row for row in selections if ends[0] < row["seconds"] < seed_ends[0]]
    if (not any(row["byte"] == DATA_SECONDARY for row in before)
            or not any(row["byte"] == STATUS_SECONDARY for row in during)
            or not any(row["byte"] == DATA_SECONDARY for row in after)):
        raise ValueError("source TALK interruption/resumption not observed")


EDGE_VARIANTS = ("structure", "precedence", "numbers", "numeric-errors", "direct", "direct-values",
                 "limits", "cache-drives", "cache-parts",
                 "cache-raw", "cache-errors", "cache-metadata", "cache-dirty",
                 "validate-cross", "validate-side")
EDGE_PHASE_START = 100
EDGE_CASE_CAPACITY = 64
EDGE_CASE_STRIDE = 256
EDGE_CASE_LIMIT = 28


def edge_steps(model, variant):
    """Cover missing bounded parser, error-order and cache transitions from the pinned ROM."""
    steps = [("edge_startup", "OPEN15,8,15", 15, 64, 73, True)]

    def command(name, text, expected):
        """Record one command's complete status before any later command can overwrite it."""
        steps.append((name, 'PRINT#15,"' + text + '"', 15, 64, expected, True))

    if variant in ("numbers", "numeric-errors", "direct", "direct-values"):
        command("initialized", "I0", 0)
    if variant in ("numbers", "numeric-errors"):
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False))
    if variant == "structure":
        # TAGCMD rejects structure before ONEDRV, PRSEQ or directory lookup.
        commands = (
            ("new_no_colon", "N0NAME", 34),
            ("new_precolon_comma", "N,0:NAME", 34),
            ("new_pattern", "N0:NA?ME", 30),
            ("new_equals", "N0:NAME=X", 30),
            ("duplicate_no_equals", "D01", 30),
            ("duplicate_same_zero", "D0=0", 30),
            ("duplicate_same_one", "D1=1", 30),
            ("duplicate_bad_target", "D2=0", 30),
            ("duplicate_bad_source", "D0=2", 30),
            ("duplicate_target_gap", "D0 =1", 30),
            ("duplicate_source_gap", "D0= 1", 30),
            ("duplicate_empty_source", "D0=", 30),
            ("duplicate_empty_target", "D=0", 30),
            ("rename_precolon_comma", "R,0:NEW=SEED", 34),
            ("rename_source_pattern", "R0:NEW=SE?D", 30),
            ("rename_trailing_group", "R0:NEW=SEED,", 30),
            ("copy_precolon_comma", "C,0:NEW=SEED", 34),
            ("copy_source_pattern", "C0:NEW=SE?D", 30),
            ("copy_target_group", "C0:NEW,OTHER=SEED", 30),
            ("scratch_precolon_comma", "S,0:SEED", 34),
            ("scratch_no_colon", "SSEED", 34),
            ("unknown_command", "X0:SEED", 31),
            ("block_no_hyphen", "B", 31),
            ("block_empty_opcode", "B-", 31),
            ("block_unknown_opcode", "B-X:2,0,A,0", 31),
        )
        for name, text, expected in commands:
            command(name, text, expected)
    elif variant == "precedence":
        steps.extend((
            ("peer_prefix", 'OPEN3,8,3,"0:SEED,P,R"', 3, 2, SEED[:2], False),
            ("direct_number", 'OPEN2,8,2,"#"', 2, 1, bytes((11,)), False),
        ))
        # BLKPAR precedes BUFTST, which precedes TSCHK. CHKIN precedes CHKIO.
        commands = (
            ("pointer_empty_numeric_before_channel", "B-P:9,A", 70),
            ("read_empty_numeric_before_channel", "B-R:9,0,A,0", 70),
            ("write_empty_numeric_before_channel", "B-W:9,0,A,0", 70),
            ("raw_read_empty_numeric_before_channel", "U1:9,0,A,0", 70),
            ("raw_write_empty_numeric_before_channel", "U2:9,0,A,0", 70),
            ("read_channel_before_geometry", "B-R:9,0,0,255", 70),
            ("write_channel_before_geometry", "B-W:9,0,0,255", 70),
            ("raw_read_channel_before_geometry", "U1:9,0,0,255", 70),
            ("raw_write_channel_before_geometry", "U2:9,0,0,255", 70),
            ("load_before_geometry", "B-R:0,0,0,255", 70),
            ("save_before_geometry", "B-W:1,0,0,255", 70),
            ("last_secondary_before_geometry", "U1:14,0,0,255", 70),
            ("allocated_read_geometry", "B-R:2,0,0,255", (66, 0, 255)),
            ("allocated_write_geometry", "B-W:2,0,255,255", (66, 255, 255)),
            ("fifth_field_before_channel", "B-R:9,0,0,0,0", 30),
            ("unknown_before_numeric", "B-X:9,A", 31),
            ("comma_before_subcommand", "B,P-:2,0", 30),
            ("rename_missing_before_collision", "R0:SEED=ABSENT", 62),
            ("rename_pattern_before_missing", "R0:SEED=AB?ENT", 30),
            ("copy_missing_before_collision", "C0:SEED=ABSENT", 62),
            ("copy_later_missing_before_collision", "C0:SEED=SEED,ABSENT", 62),
            ("copy_pattern_before_missing", "C0:SEED=AB?ENT", 30),
            ("valid_pointer_after_errors", "B-P:2,0", 0),
        )
        for name, text, expected in commands:
            command(name, text, expected)
        steps.append(("peer_retained", "", 3, 4, SEED[2:6], False))
    elif variant == "numbers":
        # ASCHEX uses the last three pseudo-decimal digits, then their low byte.
        for name, text in (
            ("read_no_colon", "B-R2,0,1,0"),
            ("read_low_bytes", "BLOCK-R:258,2,257,256"),
            ("read_omitted_sector", "B-R:2,0,1"),
            ("read_skip_character", 'B-R:' + chr(29) + "2, 0,,1,0"),
            ("raw_folded_alias", "UQ:2,0,1,0"),
            ("raw_omitted_sector", "U1:2,0,1"),
            ("read_trailing_alpha", "B-R:2,0,1,0A"),
        ):
            command(name, text, 0)
        steps.append(("raw_lower_alias", 'PRINT#15,"U"+CHR$(97)+":2,0,1,0"',
                      15, 64, 0, True))
        for name, text, expected in (
            ("pointer_omitted", "B-P:2", 0),
            ("pointer_colon_digit", "B-P:2,:", 0),
            ("pointer_question_digit", "B-P:2,?", 0),
            ("pointer_four_fields", "B-P:2,0,1,2", 0),
        ):
            command(name, text, expected)
        steps.append(("pointer_retained_hundreds", 'PRINT#15,"B-P:258,A"',
                      2, 1, bytes((200,)), False))
    elif variant == "numeric-errors":
        for name, text, expected in (
            ("pointer_five_fields", "B-P:2,0,1,2,3", 30),
            ("read_fifth_field", "B-R:2,0,1,0,0", 30),
            ("write_fifth_field", "B-W:2,0,1,0,0", 30),
            ("raw_write_fifth_field", "U2:2,0,1,0,0", 30),
            ("allocate_fifth_field", "B-A:0,1,0,0,0", 30),
            ("free_fifth_field", "B-F:0,1,0,0,0", 30),
            ("allocate_empty_numeric", "B-A:0,A,0", (66, 0, 0)),
            ("allocate_zero_track", "B-A:0,0,0", (66, 0, 0)),
            ("free_large_geometry", "B-F:0,255,255", (66, 255, 255)),
            ("read_omitted_geometry", "B-R:2", (66, 0, 0)),
            ("write_omitted_geometry", "B-W:2", (66, 0, 0)),
            ("allocate_omitted_geometry", "B-A:0", (66, 0, 0)),
            ("free_large_sector", "B-F:0,1,255", (66, 1, 255)),
        ):
            command(name, text, expected)
    elif variant == "direct":
        # OPNBLK shares ASCHEX, but reserves the explicitly selected page first.
        for index, (text, expected) in enumerate((
            ("#0", 0), ("#11", 0), ("#12", 70), ("#255", 70),
            ("#256", 0), ("#1000", 0), ("#999", 70), ("# ", 0),
            ("#, 3", 0), ("#0,1,2,3", 0), ("#0,1,2,3,4", 30),
            ("#:", 0), ("#?", 70),
        )):
            steps.append((f"direct_close_{index}", "CLOSE2", 15, 64, 0, True))
            steps.append((f"direct_name_{index}", 'OPEN2,8,2,"' + text + '"',
                          15, 64, expected, True))
    elif variant == "direct-values":
        for name, text, expected in (
            ("wrap", '"#256"', b"\0"),
            ("truncate", '"#1000"', b"\0"),
            ("colon", '"#:"', b"\x0a"),
            ("skip", '"#"+CHR$(29)+"2"', b"\x02"),
            ("three", '"#, 3"', b"\x03"),
        ):
            steps.append(("direct_close_" + name, "CLOSE2", 15, 64, 0, True))
            steps.append(("direct_page_" + name, "OPEN2,8,2," + text, 2, 1, expected, False))
        steps.append(("direct_page_busy", 'OPEN4,8,4,"#3"', 15, 64, 70, True))
        steps.append(("direct_close_busy", "CLOSE4:CLOSE2", 15, 64, 0, True))
        steps.append(("direct_page_release", 'OPEN2,8,2,"#3"', 2, 1, b"\x03", False))
        steps.append(("direct_close_return", "CLOSE2", 15, 64, 0, True))
        steps.append(("direct_return_terminator", 'OPEN2,8,2,"#11"+CHR$(13)',
                      2, 1, b"\x0b", False))
    elif variant == "limits":
        # PUT/L42 saturates at 59 bytes before CMDSET strips a terminal CR.
        for size in (57, 58, 59, 60):
            command(f"command_length_{size}", "X" * size, 31 if size <= 58 else 32)
        steps.extend((
            ("double_cr_at_limit", 'PRINT#15,"' + "X" * 58 + '"+CHR$(13)',
             15, 64, 31, True),
            ("double_cr_over_limit", 'PRINT#15,"' + "X" * 59 + '"+CHR$(13)',
             15, 64, 32, True),
            ("binary_nul_command", 'PRINT#15,CHR$(0)', 15, 64, 31, True),
            ("empty_cr_command", 'PRINT#15,""', 15, 64, 31, True),
        ))
        command("recover_after_long_command", "I0", 0)
        steps.append(("reader_after_limits", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False))
    elif variant == "cache-drives":
        command("initialize_zero", "I0", 0)
        command("initialize_foreign_one", "I1", (0, 0, 0, 1))
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("bind_direct_zero", "U1:2,0,1,0", 0)
        steps.append(("peer_one_prefix", 'OPEN3,8,3,"1:SEED,P,R"', 3, 2, SEED[:2], False))
        command("rejected_write_one", "U2:2,1,1,1", (73, 1, 1, 1))
        command("initialize_only_one", "I1", (0, 0, 0, 1))
        steps.append(("peer_one_closed", "", 3, 1, None, False))
        steps.append(("direct_zero_retained", "", 2, 1, b"\x04", False))
        command("write_zero", "U2:2,0,1,1", 0)
        command("claim_zero", "B-A:0,1,2", 0)
        steps.append(("publish_zero", "CLOSE2", 15, 64, 0, True))
    elif variant == "cache-parts":
        steps.append(("initialized", 'PRINT#15,"I0"', 15, 64, 0, True))
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("source_page", "U1:2,0,1,0", 0)
        command("initial_cached_write", "UB:2,0,1,1", (73, 1, 1) if model == "4040" else 0)
        command("select_first_part", "B-F:0,1,0", 0)
        command("foreign_cached_write", "U2:2,0,1,2", (73, 1, 2))
        last_track = {"4040": 35, "8050": 77, "8250": 154}[model]
        command("select_last_part", f"B-F:0,{last_track},0", 0)
        command("last_cached_write", "U2:2,0,1,3", (73, 1, 3) if model == "4040" else 0)
        command("initialize_discards_channel", "I0", 0)
        steps.append(("reopened", 'CLOSE2:OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("read_rejected_destination", "U1:2,0,1,2", 0)
        steps.append(("rejected_page_unchanged", "", 2, 4, bytes(4), False))
    elif variant == "cache-raw":
        steps.append(("initialized", 'PRINT#15,"I0"', 15, 64, 0, True))
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        bam_track = 18 if model == "4040" else 38
        bam_sector = {"4040": 0, "8050": 3, "8250": 9}[model]
        command("read_cached_bam_from_disk", f"U1:2,0,{bam_track},{bam_sector}", 0)
        steps.append(("edit_disk_version", 'PRINT#15,"B-P:2,2":PRINT#2,CHR$(255);:'
                      f'PRINT#15,"UB:2,0,{bam_track},{bam_sector}"', 15, 64, 0, True))
        command("read_source", "U1:2,0,1,0", 0)
        command("old_cache_still_allows_write", "U2:2,0,1,1", 0)
        command("refresh_cache", "I0", 0)
        steps.append(("reopened", 'CLOSE2:OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("reload_source", "U1:2,0,1,0", 0)
        command("new_cache_rejects_write", "U2:2,0,1,2", (73, 1, 2))
        command("read_rejected_destination", "U1:2,0,1,2", 0)
        steps.append(("rejected_page_unchanged", "", 2, 4, bytes(4), False))
    elif variant == "cache-errors":
        steps.append(("initialized", 'PRINT#15,"I0"', 15, 64, 0, True))
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        # Earlier structural/channel/geometry errors win even on foreign media.
        for name, text, expected in (
            ("empty_numeric_before_version", "B-W:2,0,A,0", (66, 0, 0)),
            ("channel_before_version", "U2:9,0,1,1", 70),
            ("geometry_before_version", "B-W:2,0,0,0", (66, 0, 0)),
            ("raw_geometry_before_version", "U2:2,0,1,255", (66, 1, 255)),
            ("write_foreign_version", "U2:2,0,1,1", (73, 1, 1)),
            ("rename_missing_before_version", "R0:SEED=ABSENT", 62),
            ("rename_exists_before_version", "R0:SEED=ONE", 63),
            ("copy_missing_before_version", "C0:SEED=ABSENT", 62),
            ("copy_exists_before_version", "C0:SEED=ONE", 63),
            ("scratch_no_match", "S0:ABSENT", (1, 0, 0)),
            ("new_syntax_before_version", "N0:NAME=X", 30),
            ("duplicate_syntax_before_version", "D0=0", 30),
        ):
            command(name, text, expected)
        steps.append(("reader_still_allowed", 'OPEN3,8,3,"0:SEED,P,R"', 3, 4, SEED[:4], False))
    elif variant == "cache-metadata":
        directory_track = 18 if model == "4040" else 39
        bam_track = 18 if model == "4040" else 38
        last_bam = {"4040": 0, "8050": 3, "8250": 9}[model]
        command("initialized", "I0", 0)
        # INITDR between failures abandons dirty RAM rather than contaminating the next case.
        for name, text, expected in (
            ("rename_foreign", "R0:NEW=SEED", (73, directory_track, 1)),
            ("scratch_foreign", "S0:ONE", (73, directory_track, 1)),
            ("copy_foreign", "C0:NEW=ONE", (73, directory_track, 1)),
            ("validate_foreign", "V0", (73, bam_track, 0)),
            ("quick_new_foreign", "N0:FOREIGN", (73, bam_track, last_bam)),
            ("duplicate_source_foreign", "D1=0", (73, bam_track, last_bam)),
        ):
            command(name, text, expected)
            command(name + "_reset", "I0", 0)
        steps.append(("source_retained", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False))
    elif variant == "cache-dirty":
        bam_track = 18 if model == "4040" else 38
        count_offset = 4 if model == "4040" else 6
        bitmap_offset = count_offset + 1
        command("initialized", "I0", 0)
        steps.append(("direct_number", 'OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("cache_claim", "B-A:0,1,1", 0)
        command("read_old_disk_map", f"U1:2,0,{bam_track},0", 0)
        steps.append(("disk_has_no_cached_claim", f'PRINT#15,"B-P:2,{bitmap_offset}"',
                      2, 1, b"\xff", False))
        steps.append(("raw_alternative_claim",
                      f'PRINT#15,"B-P:2,{count_offset}":PRINT#2,CHR$({image_sectors(model, 1) - 1});:'
                      f'PRINT#15,"B-P:2,{bitmap_offset}":PRINT#2,CHR$(251);:'
                      f'PRINT#15,"U2:2,0,{bam_track},0"', 15, 64, 0, True))
        command("read_raw_map", f"U1:2,0,{bam_track},0", 0)
        steps.append(("raw_claim_is_visible", f'PRINT#15,"B-P:2,{bitmap_offset}"',
                      2, 1, b"\xfb", False))
        steps.append(("close_publishes_cache", 'CLOSE2:OPEN2,8,2,"#"', 2, 1, b"\x0b", False))
        command("read_published_map", f"U1:2,0,{bam_track},0", 0)
        steps.append(("cache_replaces_raw_claim", f'PRINT#15,"B-P:2,{bitmap_offset}"',
                      2, 1, b"\xfd", False))
    elif variant in ("validate-cross", "validate-side"):
        command("initialized", "I0", 0)
        first = (17, 0) if model == "4040" else (38, 1)
        expected = (65, *first) if variant == "validate-cross" and model != "4040" else 0
        command("validate_result", "V0", expected)
        command("validate_repeat", "V0", expected)
        steps.append(("original_reader", 'OPEN2,8,2,"0:SEED,P,R"', 2, 4, SEED[:4], False))
    else:
        raise ValueError(f"unknown edge variant: {variant}")
    if len(steps) > EDGE_CASE_LIMIT or any(step[3] > EDGE_CASE_CAPACITY for step in steps):
        raise ValueError("edge scenario exceeds protected capture or mailbox capacity")
    return tuple(steps)


def edge_layout(model, variant):
    """Give each short edge observation isolated data, status and mailbox storage."""
    return {
        step[0]: (CAPTURE_ADDRESS + index * EDGE_CASE_STRIDE,
                  CAPTURE_ADDRESS + index * EDGE_CASE_STRIDE + EDGE_CASE_CAPACITY,
                  index + 1, EDGE_CASE_CAPACITY)
        for index, step in enumerate(edge_steps(model, variant))
    }


def edge_program(model, variant):
    """Generate bounded binary-safe edge captures using the shared PET controller."""
    steps = edge_steps(model, variant)
    return observation_program([step[1:4] for step in steps], [step[0] for step in steps],
                               EDGE_PHASE_START, edge_layout(model, variant), binary=True)


def edge_fixture(model, variant, path):
    """Seed deterministic raw pages and independent per-part version disagreements."""
    block_write_fixture(model, path)
    data = bytearray(path.read_bytes())
    entry = fixture_bam_entry(model, data, 1)
    if data[entry + 1] & 15 != 15:
        raise ValueError("edge fixture requires four free raw probe sectors")
    data[512:1024] = bytes(512)
    if variant in ("cache-parts", "cache-errors", "cache-metadata"):
        first = image_offset(model, 18 if model == "4040" else 38, 0)
        data[first + 2] = 255
        if variant in ("cache-errors", "cache-metadata") and model != "4040":
            for part in range(2 if model == "8050" else 4):
                data[image_offset(model, 38, part * 3) + 2] = 255
    if variant in ("validate-cross", "validate-side"):
        directory = image_offset(model, 18 if model == "4040" else 39, 1)
        entries = {bytes(data[offset + 5:offset + 21]).rstrip(b"\xa0"): offset
                   for offset in range(directory, directory + 256, 32)}
        if variant == "validate-cross":
            data[entries[b"ONE"] + 3:entries[b"ONE"] + 5] = data[entries[b"SEED"] + 3:entries[b"SEED"] + 5]
        else:
            data[:2] = bytes((0, 2))
            data[entries[b"ONE"] + 21:entries[b"ONE"] + 23] = bytes((1, 0))
    path.write_bytes(data)


def check_edge_media(model, variant, initial, final):
    """Require the complete source-derived raw image, including rejected destinations."""
    expected = bytearray(initial.read_bytes())
    if variant == "cache-parts" and model != "4040":
        expected[256:512] = expected[:256]
        expected[768:1024] = expected[:256]
    if variant == "cache-raw":
        bam_track = 18 if model == "4040" else 38
        bam_sector = {"4040": 0, "8050": 3, "8250": 9}[model]
        expected[image_offset(model, bam_track, bam_sector) + 2] = 255
        expected[256:512] = expected[:256]
    if variant == "cache-dirty":
        entry = fixture_bam_entry(model, expected, 1)
        expected[entry] -= 1
        expected[entry + 1] &= ~2
    if variant == "cache-drives":
        expected[256:512] = expected[:256]
        entry = fixture_bam_entry(model, expected, 1)
        expected[entry] -= 1
        expected[entry + 1] &= ~4
    if variant == "validate-side" or (variant == "validate-cross" and model == "4040"):
        expected = edge_validated_image(model, initial.read_bytes())
    if final.read_bytes() != expected:
        raise ValueError(f"edge {variant} changed bytes outside its exact complete-image expectation")


def edge_validated_image(model, initial):
    """Rebuild complete native BAM pages from the immutable fixture's directory and chains."""
    expected, used = bytearray(initial), set()

    def chain(track, sector):
        """Walk one finite raw chain, permitting the selected 4040 duplicate-owner case."""
        seen = set()
        while track:
            key = track, sector
            if key in seen:
                raise ValueError("edge validate fixture has a cyclic chain")
            seen.add(key)
            used.add(key)
            offset = image_offset(model, track, sector)
            track, sector = initial[offset:offset + 2]

    directory_track = 18 if model == "4040" else 39
    chain(directory_track, 0)
    track, sector = directory_track, 1
    while track:
        offset = image_offset(model, track, sector)
        for entry in range(offset, offset + 256, 32):
            if initial[entry + 2]:
                chain(initial[entry + 21], initial[entry + 22])
                chain(initial[entry + 3], initial[entry + 4])
        track, sector = initial[offset:offset + 2]
    tracks = {"4040": 35, "8050": 77, "8250": 154}[model]
    parts = 1 if model == "4040" else (tracks + 49) // 50
    for part in range(parts):
        offset = image_offset(model, directory_track if model == "4040" else 38, part * 3)
        page = bytearray(initial[offset:offset + 256]) if model == "4040" else bytearray(256)
        low, high = (1, 36) if model == "4040" else (part * 50 + 1, min((part + 1) * 50 + 1, tracks + 1))
        page[:2] = bytes((18, 1)) if model == "4040" else bytes((
            38 if part + 1 < parts else 39, (part + 1) * 3 if part + 1 < parts else 1))
        if model != "4040":
            page[2:6] = bytes((ord("C"), 0, low, high))
        for track in range(low, high):
            free = [sector for sector in range(image_sectors(model, track)) if (track, sector) not in used]
            index = track * 4 if model == "4040" else 6 + (track - low) * 5
            width = 3 if model == "4040" else 4
            page[index:index + width + 1] = bytes((len(free),)) + sum(
                1 << sector for sector in free).to_bytes(width, "little")
        expected[offset:offset + 256] = page
    return expected


def check_output_directory(output):
    """Prevent overwriting existing paths or staging private ROMs in tracked directories."""
    repository = Path(__file__).resolve().parents[2]
    if output.is_relative_to(repository):
        ignored = subprocess.run(
            ["git", "check-ignore", "-q", str(output)], cwd=repository, check=False,
        )
        if ignored.returncode != 0:
            raise ValueError("output inside the repository must be git-ignored (use build/)")
    output.mkdir(parents=True, exist_ok=False)


def check_machine_observation(machine, require_complete=True):
    """Require observed I/O, and reject capped logs when full capture is evidence."""
    fields = dict(line.split("=", 1) for line in machine.splitlines() if "=" in line)
    complete = fields.get("trace_complete") == "true"
    if int(fields.get("io_events", "0")) <= 0 or (require_complete and not complete):
        raise ValueError("reference controller I/O observation coverage is incomplete")
    return complete


def main():
    """Run one model, verify active ROMs and exported bytes, and retain a case manifest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=MODELS, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--drive-rom-dir", type=Path, required=True)
    parser.add_argument("--controller-rom-dir", type=Path, required=True)
    parser.add_argument("--gcr-rom", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--mame", type=Path, default=Path("/usr/games/mame"))
    parser.add_argument("--trace-build-info", type=Path)
    operator = parser.add_mutually_exclusive_group()
    operator.add_argument("--board-check", action="store_true",
                          help="verify the read-only operator executable on an original drive")
    operator.add_argument("--ordinary-check", action="store_true",
                          help="verify M3's destructive operator executable on disposable media")
    operator.add_argument("--command-check", action="store_true",
                          help="capture M4 initialize, U0, reset and invalid-command behavior")
    operator.add_argument("--rename-check", action="store_true",
                          help="capture M4 native rename parsing, metadata and open-writer behavior")
    operator.add_argument("--scratch-check", action="store_true",
                          help="capture M4 native scratch counts, patterns, types and active files")
    operator.add_argument("--copy-check", action="store_true",
                          help="capture M4 named copy, concatenate and append behavior")
    operator.add_argument("--validate-check", action="store_true",
                          help="capture M4 allocation-map repair and abandoned-writer deletion")
    operator.add_argument("--direct-check", nargs="?", const="ordinary", choices=("ordinary", "load", "save"),
                          help="capture M4 direct buffers, including LOAD/SAVE secondaries")
    operator.add_argument("--pointer-check", action="store_true",
                          help="capture M4 B-P positioning, binary data, sticky EOI and retained peers")
    operator.add_argument("--block-read-check", action="store_true",
                          help="capture M4 counted B-R, raw U1/UA, geometry errors and retained peers")
    operator.add_argument("--block-write-check", action="store_true",
                          help="capture M4 counted B-W, raw U2/UB, retained state and complete images")
    operator.add_argument("--block-map-check", action="store_true",
                          help="capture M4 B-A/B-F suggestions, exhaustion and exact BAM persistence")
    operator.add_argument("--write-version-check", choices=("foreign", "zero", "native"),
                          help="capture M4 cached-BAM version checks on counted/raw writes")
    operator.add_argument("--warm-reset-check", action="store_true",
                          help="capture M4 U9/UI warm reset, released channels and retained RAM")
    operator.add_argument("--user-interrupt-check", action="store_true",
                          help="capture M4 U;/UK IRQ entry, retained channels, cursors and maps")
    operator.add_argument("--reader-pointer-check", choices=("linked", "terminal", "block", "write"),
                          help="capture M4 ordinary-reader B-P, block reads/writes, page changes and EOI")
    operator.add_argument("--copy-all-check", nargs="?", const="all", choices=("all", "pattern"),
                          help="capture M4 whole-drive or patterned file copy with two complete images")
    operator.add_argument("--format-check", choices=("quick", "full", "duplicate"),
                          help="capture M4 NEW/FORMAT or sector DUPLICATE and exact complete media")
    operator.add_argument("--edge-check", choices=EDGE_VARIANTS,
                          help="capture M4 malformed commands, competing errors or metadata-cache interactions")
    parser.add_argument("--c1541", type=Path, default=Path("/usr/bin/c1541"))
    parser.add_argument("--petcat", type=Path, default=Path("/usr/bin/petcat"))
    args = parser.parse_args()
    if args.board_check and args.model == "8250":
        raise ValueError("operator baseline checks cover D64/D80, not unsupported legacy D82")
    output = args.output_dir.resolve()
    build_info = None
    if args.trace_build_info:
        build_info = json.loads(args.trace_build_info.read_text(encoding="ascii"))
        if (build_info["sha256"] != sha256(args.mame)
                or build_info["upstream_commit"] != MAME_COMMIT
                or build_info["patch_sha256"] != sha256(Path(__file__).with_name("drive_reference_mame.patch"))):
            raise ValueError("unverified traced MAME build")
    elif sha256(args.mame) != MAME_SHA256:
        raise ValueError("unverified MAME binary (supply pinned trace build information)")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    check_output_directory(output)
    identities = prepare_roms(args, output, manifest)
    initial, final, program = prepare_image(args, output)
    if args.board_check:
        program = output / "drive-check.prg"
    elif args.ordinary_check:
        program = output / "m3check.prg"
    before = sha256(initial)
    environment = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       ECONOPET_REFERENCE_OUTPUT=str(output),
                       ECONOPET_REFERENCE_MODEL=args.model)
    environment.pop("ECONOPET_REFERENCE_BOARD_IMAGE", None)
    if args.board_check:
        environment["ECONOPET_REFERENCE_BOARD_IMAGE"] = "64" if args.model == "4040" else "80"
    elif args.ordinary_check:
        extension = MODELS[args.model][0]
        environment["ECONOPET_REFERENCE_BOARD_IMAGE"] = "8\n0\n" + extension[1:] + "\n1"
    environment["ECONOPET_REFERENCE_DEADLINE"] = "430" if args.ordinary_check else "180"
    if build_info:
        environment["ECONOPET_IEEE_TRACE"] = str(output / "ieee-wire.csv")
    else:
        environment.pop("ECONOPET_IEEE_TRACE", None)
    command = [
        str(args.mame), "cbm4032", "-noreadconfig", "-bios", "basic4r",
        "-rompath", str(output / "roms"), "-ieee8", "c" + args.model,
        "-flop1", str(final), "-quik", str(program),
        "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
        "-autoboot_delay", "5", "-autoboot_script",
        str(Path(__file__).with_suffix(".lua").resolve()),
        "-seconds_to_run", "435" if args.ordinary_check else "185", "-cfg_directory", str(output / "cfg"),
        "-nvram_directory", str(output / "nvram"),
    ]
    if args.copy_all_check or args.format_check == "duplicate" or args.edge_check == "cache-drives":
        command.extend(("-flop2", str(output / ("source-final." + MODELS[args.model][0]))))
    run(command, output / "mame.log", env=environment)
    _, parts, controller = MODELS[args.model]
    expected_main = b"".join((args.drive_rom_dir / (part + ".bin")).read_bytes()
                             for part in parts)
    if (output / "active-main.bin").read_bytes() != expected_main:
        raise ValueError("executed main-DOS ROM differs from approved inputs")
    if (output / "active-controller.bin").read_bytes() != (
            args.drive_rom_dir / (controller + ".bin")).read_bytes():
        raise ValueError("executed controller ROM differs from approved input")
    machine = (output / "machine.txt").read_text(encoding="ascii")
    controller_log_complete = check_machine_observation(machine, require_complete=not args.ordinary_check)
    if args.ordinary_check:
        mailbox = (output / "mailbox.bin").read_bytes()
        if mailbox[:3] != bytes((COMPLETE, 13, 13)) or sha256(final) == before:
            raise ValueError("M3 operator executable did not pass 13/13 on changed disposable media")
        saved = output / "m3save.prg"
        run([str(args.c1541), "-attach", str(final), "-read", "m3save,p", str(saved)],
            output / "m3save.log")
        if saved.read_bytes() != program.read_bytes():
            raise ValueError("M3 operator SAVE did not persist the complete program")
        result = {
            "schema": 1, "kind": "operator-ordinary-files-native-check", "model": args.model,
            "roms": identities, "mame_sha256": sha256(args.mame), "command": command,
            "program_sha256": sha256(program), "passed": 13,
            "initial_sha256": before, "final_sha256": sha256(final),
            "controller_log_complete": controller_log_complete,
            "limits": ["operator-checker validation, not a full controller-register or wire comparison"],
        }
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
        print(f"{args.model}: M3 operator executable passed 13/13 ({output})")
        return
    if args.board_check:
        mailbox = (output / "mailbox.bin").read_bytes()
        if mailbox[:2] != bytes((COMPLETE, 6)) or sha256(final) != before:
            raise ValueError("operator executable failed native checks or changed its read-only image")
        result = {
            "schema": 1, "kind": "operator-program-native-check", "model": args.model,
            "roms": identities, "mame_sha256": sha256(args.mame),
            "program_sha256": sha256(program), "image_sha256": before, "passed": 6,
        }
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
        print(f"{args.model}: operator executable passed 6/6 on original-ROM reference ({output})")
        return
    observations = decode_capture((output / "capture.bin").read_bytes(),
                                  (output / "mailbox.bin").read_bytes(),
                                  edge_layout(args.model, args.edge_check) if args.edge_check else
                                  format_layout(args.format_check) if args.format_check else
                                  copy_all_layout(args.copy_all_check) if args.copy_all_check else READER_POINTER_LAYOUTS[args.reader_pointer_check] if args.reader_pointer_check else USER_INTERRUPT_LAYOUT if args.user_interrupt_check else WARM_RESET_LAYOUT if args.warm_reset_check else WRITE_VERSION_LAYOUT if args.write_version_check else BLOCK_MAP_LAYOUT if args.block_map_check else BLOCK_WRITE_LAYOUT if args.block_write_check else BLOCK_READ_LAYOUT if args.block_read_check else POINTER_LAYOUT if args.pointer_check else DIRECT_LAYOUT if args.direct_check else VALIDATE_LAYOUT if args.validate_check else COPY_LAYOUT if args.copy_check else SCRATCH_LAYOUT if args.scratch_check else RENAME_LAYOUT if args.rename_check
                                  else COMMAND_LAYOUT if args.command_check else LAYOUT)
    if args.edge_check:
        check_block_observations(args.model, observations, edge_steps(args.model, args.edge_check),
                                 edge_layout(args.model, args.edge_check), "edge-" + args.edge_check)
        check_edge_media(args.model, args.edge_check, initial, final)
        if args.edge_check == "cache-drives" and (
                output / ("source-initial." + MODELS[args.model][0])).read_bytes() != (
                output / ("source-final." + MODELS[args.model][0])).read_bytes():
            raise ValueError("rejected cross-drive writes changed the complete foreign image")
    elif args.format_check:
        check_block_observations(args.model, observations, format_steps(args.model, args.format_check),
                                 format_layout(args.format_check), "format")
        source = output / ("source-initial." + MODELS[args.model][0])
        check_format_media(args.model, args.format_check, initial, final, source)
        if args.format_check == "duplicate":
            check_copy_all_source(source, output / ("source-final." + MODELS[args.model][0]))
    elif args.copy_all_check:
        check_copy_all_observations(args.model, observations, args.copy_all_check)
        check_copy_all_source(output / ("source-initial." + MODELS[args.model][0]),
                              output / ("source-final." + MODELS[args.model][0]))
        payloads = {
            "one,s": b"A", "bound254,s": bytes(range(254)),
            "bound255,s": bytes(range(255)), "seed,p": SEED, "extra,s": b"A",
        } if args.copy_all_check == "pattern" else {
            "seed,p": SEED, "one,s": b"A", "bound254,s": bytes(range(254)),
            "bound255,s": bytes(range(255)), "m1check,p": (output / "drive-check.prg").read_bytes(),
            "extra,s": b"A",
        }
        for number, (name, payload) in enumerate(payloads.items()):
            destination = output / f"copy-all-{number}.bin"
            run([str(args.c1541), "-attach", str(final), "-read", name, str(destination)],
                output / f"copy-all-extract-{number}.log")
            if destination.read_bytes() != payload:
                raise ValueError(f"independently extracted copy-all payload mismatch: {name}")
    elif args.reader_pointer_check:
        check_reader_pointer_observations(args.model, args.reader_pointer_check, observations, initial)
        if args.reader_pointer_check == "write":
            check_reader_write_media(args.model, initial, final)
        else:
            check_reader_pointer_media(initial, final)
    elif args.user_interrupt_check:
        check_user_interrupt_observations(args.model, observations)
        check_user_interrupt_media(args.model, initial, final)
    elif args.warm_reset_check:
        check_warm_reset_observations(args.model, observations)
        check_warm_reset_media(args.model, initial, final)
    elif args.write_version_check:
        check_write_version_observations(args.model, args.write_version_check, observations)
        check_write_version_media(args.model, args.write_version_check, initial, final)
    elif args.block_map_check:
        check_block_map_observations(args.model, observations)
        check_block_map_media(args.model, initial, final)
    elif args.block_write_check:
        check_block_write_observations(args.model, observations)
        check_block_write_media(args.model, initial, final)
    elif args.block_read_check:
        check_block_read_observations(args.model, observations)
    elif args.pointer_check:
        check_pointer_observations(args.model, observations)
    elif args.direct_check:
        check_direct_observations(args.model, observations, args.direct_check)
    elif args.validate_check:
        check_validate_observations(args.model, observations)
        check_validate_media(args.model, initial, final)
        for number, (name, expected) in enumerate({"seed,p": SEED, "new,p": SEED,
                                                 "one,s": b"A", "bound254,s": bytes(range(254)),
                                                 "m1check,p": (output / "drive-check.prg").read_bytes()}.items()):
            destination = output / f"validate-{number}.bin"
            run([str(args.c1541), "-attach", str(final), "-read", name, str(destination)],
                output / f"extract-{number}.log")
            if destination.read_bytes() != expected:
                raise ValueError(f"independently extracted validate payload mismatch: {name}")
    elif args.copy_check:
        check_copy_observations(args.model, observations)
        for number, (name, expected) in enumerate(COPY_PAYLOADS.items()):
            destination = output / f"copy-{number}.bin"
            run([str(args.c1541), "-attach", str(final), "-read", name, str(destination)],
                output / f"extract-{number}.log")
            if destination.read_bytes() != expected:
                raise ValueError(f"independently extracted copy payload mismatch: {name}")
    elif args.scratch_check:
        check_scratch_observations(args.model, observations)
        run([str(args.c1541), "-attach", str(final), "-read", "m1check,p",
             str(output / "persisted.prg")], output / "extract.log")
        if (output / "persisted.prg").read_bytes() != (output / "drive-check.prg").read_bytes():
            raise ValueError("scratch changed the independently extracted surviving PRG")
    elif args.rename_check:
        check_rename_observations(args.model, observations)
        run([str(args.c1541), "-attach", str(final), "-read", "restored,p",
             str(output / "persisted.prg")], output / "extract.log")
        if (output / "persisted.prg").read_bytes() != SEED:
            raise ValueError("rename changed the independently extracted PRG payload")
    elif args.command_check:
        check_command_observations(args.model, observations)
    else:
        check_observations(args.model, observations)
        run([str(args.c1541), "-attach", str(final), "-read", "result,s",
             str(output / "persisted.seq")], output / "extract.log")
        if (output / "persisted.seq").read_bytes() != WRITTEN:
            raise ValueError("persisted file mismatch")
    if sha256(initial) != before:
        raise ValueError("read-only input changed")
    old, new = initial.read_bytes(), final.read_bytes()
    if len(old) != len(new):
        raise ValueError("exported image changed container geometry")
    changed = [offset // 256 for offset in range(0, len(old), 256)
               if old[offset:offset + 256] != new[offset:offset + 256]]
    if not args.edge_check and (args.reader_pointer_check in ("linked", "terminal", "block") or args.command_check or args.direct_check or args.pointer_check or args.block_read_check or args.write_version_check == "foreign" or (args.warm_reset_check and args.model != "4040")) and changed:
        raise ValueError("read-only command sequence changed the persisted image")
    if not args.edge_check and args.reader_pointer_check not in ("linked", "terminal", "block") and not args.command_check and not args.direct_check and not args.pointer_check and not args.block_read_check and args.write_version_check != "foreign" and not (args.warm_reset_check and args.model != "4040") and not changed:
        raise ValueError("native write did not change the persisted image")
    with (output / "phases.csv").open(encoding="ascii") as stream:
        phases = list(csv.DictReader(stream))
    commands = controller_commands(output / "controller-io.csv")
    if not args.edge_check and not args.format_check and not args.copy_all_check and not args.reader_pointer_check and not args.command_check and not args.rename_check and not args.scratch_check and not args.copy_check and not args.validate_check and not args.direct_check and not args.pointer_check and not args.block_read_check and not args.block_write_check and not args.block_map_check and not args.write_version_check and not args.warm_reset_check and not args.user_interrupt_check:
        check_interruption(commands, phases)
    wire = None
    if build_info:
        starts = [row["seconds"] for row in phases if row["phase"] == "0"]
        if len(starts) != 1:
            raise ValueError("missing or repeated native stimulus start marker")
        wire = decode_wire(output / "ieee-wire.csv", attoseconds(starts[0]))
        wire["domain"] = "mame-ieee488-composed"
        if args.edge_check or args.format_check or args.copy_all_check or args.reader_pointer_check or args.command_check or args.rename_check or args.scratch_check or args.copy_check or args.validate_check or args.direct_check or args.pointer_check or args.block_read_check or args.block_write_check or args.block_map_check or args.write_version_check or args.warm_reset_check or args.user_interrupt_check:
            edge_phases = {0: "setup", **{EDGE_PHASE_START + index: step[0]
                           for index, step in enumerate(edge_steps(args.model, args.edge_check))}} if args.edge_check else None
            label_cases(wire["frames"], phases, edge_phases if args.edge_check else FORMAT_PHASES if args.format_check else (COPY_PATTERN_PHASES if args.copy_all_check == "pattern" else COPY_ALL_PHASES) if args.copy_all_check else READER_POINTER_PHASES[args.reader_pointer_check] if args.reader_pointer_check else USER_INTERRUPT_PHASES if args.user_interrupt_check else WARM_RESET_PHASES if args.warm_reset_check else WRITE_VERSION_PHASES if args.write_version_check else BLOCK_MAP_PHASES if args.block_map_check else BLOCK_WRITE_PHASES if args.block_write_check else BLOCK_READ_PHASES if args.block_read_check else POINTER_PHASES if args.pointer_check else DIRECT_PHASES if args.direct_check else VALIDATE_PHASES if args.validate_check else COPY_PHASES if args.copy_check else SCRATCH_PHASES if args.scratch_check
                        else RENAME_PHASES if args.rename_check else COMMAND_PHASES)
            # Timeout CR is synthesized by the PET, not a drive wire transfer.
            accepted = {
                name: {key: [value for value, status in zip(values, sample["kernal_st"])
                             if not status & 2] for key, values in sample.items()}
                for name, sample in observations.items()
            }
            check_wire_bytes(wire, accepted)
        else:
            label_cases(wire["frames"], phases)
            check_wire_bytes(wire, observations)
        write_replay(output / "ieee-wire.csv", wire, output / "replay.txt")
    result = {
        "schema": 1, "model": args.model, "controller": "cbm4032/basic4r",
        "scenario": "edge-" + args.edge_check if args.edge_check else "format-" + args.format_check if args.format_check else "copy-pattern" if args.copy_all_check == "pattern" else "copy-all" if args.copy_all_check else "reader-pointer-" + args.reader_pointer_check if args.reader_pointer_check else "user-interrupt" if args.user_interrupt_check else "warm-reset" if args.warm_reset_check else "write-version-" + args.write_version_check if args.write_version_check else "block-map" if args.block_map_check else "block-write" if args.block_write_check else "block-read" if args.block_read_check else "buffer-pointer" if args.pointer_check else "direct-buffer" if args.direct_check else "validate" if args.validate_check else "copy" if args.copy_check else "scratch" if args.scratch_check else "rename" if args.rename_check else
                    "initialize-utility-reset" if args.command_check else "ordinary-files-reset",
        "emulator": {"version": "0.285" if build_info else "0.285+dfsg1-1",
                     "upstream_commit": MAME_COMMIT,
                     "sha256": sha256(args.mame)},
        "roms": identities, "approval_manifest_sha256": sha256(args.manifest),
        "tools": {name: sha256(getattr(args, name)) for name in ("c1541", "petcat")},
        "command": command, "observations": observations, "phases": phases,
        "controller_commands": commands,
        "wire": wire, "trace_build": build_info,
        "machine": machine,
        "artifacts": {name: sha256(output / name) for name in (
            "controller.prg", "controller-io.csv", "phases.csv",
            "capture.bin", "mailbox.bin")},
        "harness": {name: sha256(Path(__file__).with_name(name)) for name in (
            "drive_reference.py", "drive_reference.lua", "drive_wire.py")},
        "media": {"initial_sha256": before, "final_sha256": sha256(final),
                  "size": len(new), "changed_sectors": changed,
                  "persisted_file_sha256": sha256(output / "persisted.prg") if args.rename_check or args.scratch_check
                                          else None if args.edge_check or args.format_check or args.copy_all_check or args.reader_pointer_check or args.command_check or args.copy_check or args.validate_check or args.direct_check or args.pointer_check or args.block_read_check or args.block_write_check or args.block_map_check or args.write_version_check or args.warm_reset_check or args.user_interrupt_check else sha256(output / "persisted.seq"),
                  "copied_file_sha256": {name: sha256(output / f"copy-{number}.bin")
                                        for number, name in enumerate(COPY_PAYLOADS)} if args.copy_check else {}},
        "limits": [
            "PET BASIC operation times include interpreter/controller overhead",
            "exact composed-net trace available only when wire is non-null",
            "physical timing and storage-fault parity are not established",
            "MAME FDC write-protect handling is marked TODO",
        ],
    }
    if args.direct_check in ("load", "save"):
        result["scenario"] = "direct-" + args.direct_check
    if args.copy_all_check or args.format_check == "duplicate" or args.edge_check == "cache-drives":
        extension = MODELS[args.model][0]
        source_initial, source_final = "source-initial." + extension, "source-final." + extension
        result["media"]["secondary"] = {
            "slot": 1, "initial": source_initial, "final": source_final,
            "initial_sha256": sha256(output / source_initial),
            "final_sha256": sha256(output / source_final),
            "size": (output / source_initial).stat().st_size,
        }
    if wire:
        result["artifacts"].update({name: sha256(output / name)
                                    for name in ("ieee-wire.csv", "replay.txt")})
        result["harness"]["drive_reference_mame.patch"] = sha256(
            Path(__file__).with_name("drive_reference_mame.patch"))
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="ascii")
    evidence = ("command edges and exact complete images" if args.edge_check else
                "NEW/FORMAT/DUPLICATE and exact complete images" if args.format_check else
                "patterned directory-copy cases and both complete images" if args.copy_all_check == "pattern" else
                "colonless whole-drive copy cases and both complete images" if args.copy_all_check else
                "ordinary-reader B-W/U2/UB cases and exact complete media" if args.reader_pointer_check == "write" else
                "ordinary-reader B-R/U1 cases and unchanged media" if args.reader_pointer_check == "block" else
                "ordinary-reader B-P cases and unchanged media" if args.reader_pointer_check else
                "user-interrupt cases and retained channel/map persistence" if args.user_interrupt_check else
                "warm-reset cases and retained BAM persistence" if args.warm_reset_check else
                "write-version cases and exact accepted/rejected media" if args.write_version_check else
                "block-map cases and exact persisted BAM" if args.block_map_check else
                "block-write cases and exact persisted page" if args.block_write_check else
                "block-read cases and unchanged media" if args.block_read_check else
                "buffer-pointer cases and unchanged media" if args.pointer_check else
                "direct-buffer cases and unchanged media" if args.direct_check else
                "validate cases, complete rebuilt maps and five persisted payloads" if args.validate_check else
                "named copy cases and ten persisted payloads" if args.copy_check else
                "scratch cases and surviving payload" if args.scratch_check else
                "rename cases and persisted payload" if args.rename_check else
                "command cases and unchanged media" if args.command_check else "cases and persisted write")
    print(f"{args.model}: original-ROM {evidence} verified ({output})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
