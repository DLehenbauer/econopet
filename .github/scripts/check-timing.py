#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Fail when an Efinity timing report contains negative setup or hold slack."""

import math
import re
import sys
from pathlib import Path


SECTION_BEGIN = "---------- 2. Clock Relationship Summary (begin) ----------"
SECTION_END = "---------- Clock Relationship Summary (end) ---------------"
SETUP_HEADING = "Setup (Max) Clock Relationship"
HOLD_HEADING = "Hold (Min) Clock Relationship"
COLUMN_HEADER = (
    "Launch Clock Capture Clock Constraint (ns) Slack (ns) Edge".split()
)
NOTE = "NOTE: Values are in nanoseconds."
EDGE_PATTERN = re.compile(r"^\([RF]-[RF]\)$")


class TimingReportError(ValueError):
    pass


def parse_table(
    lines: list[str], relationship_type: str
) -> list[tuple[float, str, str]]:
    if not lines or lines[0].split() != COLUMN_HEADER:
        raise TimingReportError(
            f"unexpected {relationship_type} clock relationship header"
        )

    relationships = []
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 5 or not EDGE_PATTERN.fullmatch(fields[-1]):
            raise TimingReportError(
                f"unexpected {relationship_type} clock relationship row: {line}"
            )

        try:
            constraint, slack = map(float, fields[2:4])
        except ValueError as error:
            raise TimingReportError(
                f"invalid number in {relationship_type} clock relationship: {line}"
            ) from error

        if not math.isfinite(constraint) or not math.isfinite(slack):
            raise TimingReportError(
                f"non-finite number in {relationship_type} clock relationship: {line}"
            )

        relationships.append((slack, fields[0], fields[1]))

    if not relationships:
        raise TimingReportError(
            f"no {relationship_type} clock relationships found"
        )

    return relationships


def parse_relationships(report: Path) -> dict[str, list[tuple[float, str, str]]]:
    lines = [
        line.strip()
        for line in report.read_text(encoding="utf-8").splitlines()
    ]
    if lines.count(SECTION_BEGIN) != 1 or lines.count(SECTION_END) != 1:
        raise TimingReportError(
            "expected exactly one complete clock relationship summary"
        )

    section_begin = lines.index(SECTION_BEGIN)
    section_end = lines.index(SECTION_END)
    if section_begin >= section_end:
        raise TimingReportError("clock relationship summary markers are out of order")

    summary = [
        line for line in lines[section_begin + 1 : section_end] if line
    ]
    if summary.count(SETUP_HEADING) != 1 or summary.count(HOLD_HEADING) != 1:
        raise TimingReportError(
            "expected exactly one setup and one hold clock relationship table"
        )

    setup_heading = summary.index(SETUP_HEADING)
    hold_heading = summary.index(HOLD_HEADING)
    if setup_heading != 0 or hold_heading <= setup_heading:
        raise TimingReportError("clock relationship tables are out of order")
    if summary[-1] != NOTE:
        raise TimingReportError("missing clock relationship units note")

    return {
        "setup": parse_table(summary[setup_heading + 1 : hold_heading], "setup"),
        "hold": parse_table(summary[hold_heading + 1 : -1], "hold"),
    }


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} TIMING_REPORT", file=sys.stderr)
        return 2

    report = Path(sys.argv[1])
    if not report.is_file():
        print(f"error: Efinity timing report not found: {report}", file=sys.stderr)
        return 1

    try:
        relationships = parse_relationships(report)
    except (OSError, UnicodeError, TimingReportError) as error:
        print(f"error: failed to parse {report}: {error}", file=sys.stderr)
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
