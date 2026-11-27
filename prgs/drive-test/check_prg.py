#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check that the complete tape-loaded PRG fits in PET base RAM."""

import re
import sys
from pathlib import Path


MAILBOX_ADDR = 0x7FF0
PRG_HEADER_SIZE = 2
DEFAULT_STACK_SIZE = 0x800
SEGMENT = re.compile(
    r"^(\w+)\s+([0-9A-Fa-f]{6})\s+([0-9A-Fa-f]{6})\s+([0-9A-Fa-f]{6})\s+",
    re.MULTILINE,
)
STACKSIZE_EXPORT = re.compile(r"__STACKSIZE__\s+([0-9A-Fa-f]{6})\s+REA")


def main(path: Path, map_path: Path) -> int:
    """Reject malformed launchers or resident segments overlapping the stack or mailbox."""
    # Read the compiled PRG binary and ensure it carries at least one payload byte past its 2-byte header.
    data = path.read_bytes()
    if len(data) <= PRG_HEADER_SIZE:
        raise ValueError(f"{path}: missing PRG payload")

    # Determine the CC65 software stack size from the linker map and reserve space below the $7FF0 mailbox.
    map_text = map_path.read_text()
    stack_match = STACKSIZE_EXPORT.search(map_text)
    stack_size = int(stack_match.group(1), 16) if stack_match else DEFAULT_STACK_SIZE
    if stack_size < 0x100 or stack_size >= MAILBOX_ADDR - 0x0401:
        raise ValueError(f"{map_path}: invalid stack size ${stack_size:04X}")
    resident_limit = MAILBOX_ADDR - stack_size

    # Verify that the PRG loads at standard PET BASIC start ($0401) and ends below the stack region.
    start = int.from_bytes(data[:PRG_HEADER_SIZE], "little")
    end = start + len(data) - PRG_HEADER_SIZE
    if start != 0x0401 or end > resident_limit:
        raise ValueError(
            f"{path}: loaded range ${start:04X}-${end - 1:04X} "
            f"does not fit below ${resident_limit:04X}"
        )

    # Parse the CC65 segment table and verify that every resident segment (including BSS) fits below the stack.
    table = map_text.split("Segment list:", 1)
    if len(table) != 2:
        raise ValueError(f"{map_path}: missing segment list")
    table = table[1].split("Exports list by name:", 1)[0]
    segments = {
        name: (int(first, 16), int(last, 16), int(size, 16))
        for name, first, last, size in SEGMENT.findall(table)
    }
    if not {"EXEHDR", "STARTUP", "CODE", "BSS"}.issubset(segments):
        raise ValueError(f"{map_path}: missing resident segments")
    for name, (first, last, size) in segments.items():
        if name in ("ZEROPAGE", "LOADADDR"):
            continue
        if first < start or last >= resident_limit or size != last - first + 1:
            raise ValueError(f"{map_path}: invalid resident segment {name}")

    # Validate the tokenized BASIC 'SYS <startup>' stub so 'RUN' jumps directly to CC65's STARTUP segment.
    payload = data[PRG_HEADER_SIZE:]
    terminator = payload.find(b"\0", 5)
    if (
        len(payload) < 12
        or payload[4] != 0x9E
        or terminator < 0
        or payload[terminator + 1:terminator + 3] != b"\0\0"
        or int.from_bytes(payload[:2], "little") != start + terminator + 1
        or not payload[5:terminator].isdigit()
        or int(payload[5:terminator]) != segments["STARTUP"][0]
    ):
        raise ValueError(f"{path}: invalid BASIC SYS launcher")

    # Report the verified load span, resident BSS ceiling, and reserved CC65 stack base.
    resident_end = max(last for name, (_, last, _) in segments.items()
                       if name not in ("ZEROPAGE", "LOADADDR")) + 1
    print(
        f"Drive Test PRG: ${start:04X}-${end - 1:04X} ({len(data)} bytes), "
        f"resident through ${resident_end - 1:04X}, "
        f"{stack_size} byte stack at ${resident_limit:04X}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2])))
    except (OSError, ValueError, IndexError) as error:
        print(f"Drive Test PRG check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
