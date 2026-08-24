#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Fail when an Efinity timing report contains negative setup or hold slack."""

import re
import sys
from pathlib import Path


SECTION_BEGIN = "---------- 2. Clock Relationship Summary (begin) ----------"
SECTION_END = "---------- Clock Relationship Summary (end) ---------------"
EDGE_PATTERN = re.compile(r"^\([RF]-[RF]\)$")


def parse_relationships(report: Path) -> dict[str, list[tuple[float, str, str]]]:
    relationships: dict[str, list[tuple[float, str, str]]] = {
        "setup": [],
        "hold": [],
    }
    in_summary = False
    relationship_type: str | None = None

    with report.open(encoding="utf-8") as report_file:
        for line in report_file:
            stripped = line.strip()

            if stripped == SECTION_BEGIN:
                in_summary = True
                continue
            if stripped == SECTION_END:
                break
            if not in_summary:
                continue

            if stripped == "Setup (Max) Clock Relationship":
                relationship_type = "setup"
                continue
            if stripped == "Hold (Min) Clock Relationship":
                relationship_type = "hold"
                continue

            fields = stripped.split()
            if (
                relationship_type is None
                or len(fields) < 5
                or not EDGE_PATTERN.fullmatch(fields[-1])
            ):
                continue

            try:
                slack = float(fields[-2])
                float(fields[-3])
            except ValueError:
                continue

            relationships[relationship_type].append((slack, fields[0], fields[1]))

    return relationships


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} TIMING_REPORT", file=sys.stderr)
        return 2

    report = Path(sys.argv[1])
    if not report.is_file():
        print(f"error: Efinity timing report not found: {report}", file=sys.stderr)
        return 1

    relationships = parse_relationships(report)
    missing = [kind for kind, entries in relationships.items() if not entries]
    if missing:
        print(
            f"error: no {' or '.join(missing)} clock relationships found in {report}",
            file=sys.stderr,
        )
        return 1

    failed = False
    for kind, entries in relationships.items():
        slack, launch_clock, capture_clock = min(entries)
        print(
            f"Worst {kind} slack: {slack:.3f} ns "
            f"({launch_clock} -> {capture_clock})"
        )
        if slack < 0:
            failed = True

    if failed:
        print("error: gateware failed timing closure", file=sys.stderr)
        return 1

    print("Gateware passed timing closure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
