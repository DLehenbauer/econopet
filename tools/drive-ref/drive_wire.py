# SPDX-License-Identifier: CC0-1.0
"""Decode complete composed-net traces without confusing DAV with byte acceptance."""

from bisect import bisect_right
import csv
from decimal import Decimal
from pathlib import Path


ATTOSECONDS = 10**18
LINES = ("atn", "eoi", "dav", "nrfd", "ndac", "ifc", "dio")
EVENT_LIMIT = 2_000_000
PHASE_CASES = {
    0: "setup", 1: "startup", 2: "startup", 3: "consumed", 4: "consumed",
    5: "seed", 6: "seed", 9: "interposed", 10: "seed",
    11: "onebyte", 12: "onebyte", 13: "boundary254", 14: "boundary254",
    15: "boundary255", 16: "boundary255", 17: "write", 18: "write",
    19: "written_status", 20: "written_status", 21: "readback",
    22: "reset_status", 23: "reset_status", 24: "reset_status",
}


def attoseconds(text):
    """Parse decimal emulator time without losing ordering or sub-microsecond phases."""
    value = Decimal(text) * ATTOSECONDS
    if not value.is_finite() or value < 0 or value != value.to_integral_value():
        raise ValueError("invalid IEEE timestamp")
    return int(value)


def load_trace(path):
    """Require contiguous, bounded source coverage with initial and terminal snapshots."""
    rows = []
    with Path(path).open(encoding="ascii") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["sequence", "seconds", "event", "source", *LINES]:
            raise ValueError("invalid IEEE trace header")
        for expected, raw in enumerate(reader, 1):
            if expected > EVENT_LIMIT:
                raise ValueError("IEEE trace exceeds event limit")
            row = {key: int(raw[key]) for key in ("sequence", *LINES)}
            row.update(time_as=attoseconds(raw["seconds"]), event=raw["event"],
                       source=raw["source"])
            if row["sequence"] != expected or row["event"] not in ("initial", "edge", "stop"):
                raise ValueError("incomplete IEEE sequence coverage")
            if any(row[line] not in (0, 1) for line in LINES[:-1]) or not 0 <= row["dio"] <= 255:
                raise ValueError("invalid composed IEEE levels")
            if rows and row["time_as"] < rows[-1]["time_as"]:
                raise ValueError("reversed IEEE timestamps")
            rows.append(row)
    if (len(rows) < 2 or rows[0]["event"] != "initial" or rows[-1]["event"] != "stop"
            or any(row["event"] != "edge" for row in rows[1:-1])
            or any(rows[-1][line] != rows[-2][line] for line in LINES)):
        raise ValueError("incomplete IEEE observation coverage")
    return rows


def decode_wire(path, start_as=None):
    """Measure accepted transfers, aborted DAV pulses and individual handshake phases."""
    rows = load_trace(path)
    if start_as is not None:
        before = [row for row in rows if row["time_as"] <= start_as]
        if not before or start_as >= rows[-1]["time_as"]:
            raise ValueError("IEEE stimulus window is outside complete source coverage")
        initial = dict(before[-1], time_as=start_as, event="initial")
        rows = [initial, *(row for row in rows if row["time_as"] > start_as)]
    frames, aborted, issues = [], [], []
    pending = None
    previous = rows[0]
    ready_since = None
    last_released = None
    talking = False
    if not previous["dav"]:
        issues.append({"sequence": previous["sequence"], "issue": "DAV active at observation start"})
    for row in rows[1:]:
        now = row["time_as"]
        if not previous["nrfd"] and row["nrfd"]:
            ready_since = now
        if previous["nrfd"] and not row["nrfd"]:
            ready_since = None
        if previous["dav"] and not row["dav"]:
            pending = {
                "sequence": row["sequence"], "dav_as": now, "byte": row["dio"] ^ 255,
                "atn": not bool(row["atn"]), "eoi": not bool(row["eoi"]),
                "direction": "drive" if row["atn"] and talking else "host",
                "ready_to_dav_as": None if ready_since is None else now - ready_since,
            }
            if not previous["nrfd"] or previous["ndac"]:
                issues.append({"sequence": row["sequence"], "issue": "DAV before listener ready/rearmed"})
        if pending and "accept_as" not in pending:
            interrupted = (previous["atn"] and not row["atn"]) or not row["ifc"]
            if interrupted:
                aborted.append(dict(pending, abort_as=now, reason="ATN/IFC interruption"))
                pending = None
            elif row["dio"] != (pending["byte"] ^ 255) or bool(not row["eoi"]) != pending["eoi"]:
                issues.append({"sequence": row["sequence"], "issue": "data/EOI changed before acceptance"})
        if pending and not previous["ndac"] and row["ndac"] and not row["dav"]:
            pending["accept_as"] = now
            pending["dav_to_accept_as"] = now - pending["dav_as"]
        if not previous["dav"] and row["dav"] and pending:
            if "accept_as" not in pending:
                aborted.append(dict(pending, abort_as=now, reason="DAV released without acceptance"))
            else:
                pending["release_as"] = now
                pending["accept_to_release_as"] = now - pending["accept_as"]
                frames.append(pending)
                last_released = pending
                # The accepted command, not a speculative DIO write, changes direction.
                if pending["atn"]:
                    command = pending["byte"]
                    if 0x40 <= command <= 0x5e:
                        talking = True
                    elif command == 0x5f or 0x20 <= command <= 0x3e:
                        talking = False
            pending = None
        if previous["ndac"] and not row["ndac"] and last_released is not None:
            last_released["release_to_rearm_as"] = now - last_released["release_as"]
            last_released = None
        previous = row
    if pending:
        issues.append({"sequence": pending["sequence"], "issue": "in-flight DAV at observation end"})
    return {
        "coverage": {"start_as": rows[0]["time_as"], "end_as": rows[-1]["time_as"],
                     "events": len(rows), "complete": True},
        "frames": frames, "aborted": aborted, "issues": issues,
    }


def label_cases(frames, phases, cases=PHASE_CASES):
    """Bind each accepted DAV interval to the native controller's active case."""
    times = [attoseconds(row["seconds"]) for row in phases]
    if times != sorted(times):
        raise ValueError("reversed controller phases")
    for frame in frames:
        index = bisect_right(times, frame["dav_as"]) - 1
        phase = 0 if index < 0 else int(phases[index]["phase"])
        frame["case"] = cases[phase]


def check_wire_bytes(wire, observations):
    """Cross-check actual accepted drive bytes and EOI against independent PET captures."""
    for case, expected in observations.items():
        frames = [frame for frame in wire["frames"]
                  if frame["case"] == case and frame["direction"] == "drive"]
        if ([frame["byte"] for frame in frames] != expected["bytes"]
                or [frame["eoi"] for frame in frames]
                != [bool(value & 0x40) for value in expected["kernal_st"]]):
            raise ValueError(f"accepted wire bytes/EOI disagree with PET capture: {case}")


def write_replay(trace, wire, destination):
    """Export accepted transactions and controller ATN release in original source order."""
    frames = {frame["sequence"]: frame for frame in wire["frames"]}
    actions = []
    previous = None
    for row in load_trace(trace):
        if row["time_as"] < wire["coverage"]["start_as"]:
            previous = row
            continue
        frame = frames.get(row["sequence"])
        if frame:
            actions.append(f'{frame["case"]} {"R" if frame["direction"] == "drive" else "H"} '
                           f'{frame["byte"]} {int(frame["atn"])} {int(frame["eoi"])}')
        if previous and not previous["atn"] and row["atn"]:
            actions.append("control A 0 0 0")
        previous = row
    if wire["aborted"] or wire["issues"]:
        raise ValueError("cannot replay an interrupted or invalid reference handshake")
    Path(destination).write_text("ieee-replay-v1\n" + "\n".join(actions) + "\n", encoding="ascii")
    return len(actions)
