#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Compare original-ROM transactions with Debug firmware through production SPI/FPGA."""

import argparse
import csv
import json
from pathlib import Path
import subprocess

from drive_reference import MODELS, check_output_directory, sha256
from drive_wire import decode_wire


TIMED_PHASES = ("ready_to_dav_as", "dav_to_accept_as", "accept_to_release_as",
                "release_to_rearm_as")


def compare_frames(expected, actual):
    """Compare every accepted byte/ATN/EOI and retain unadjusted phase timings."""
    differences, timings = [], []
    for index, (native, candidate) in enumerate(zip(expected, actual)):
        for field in ("byte", "atn", "eoi", "direction"):
            if native[field] != candidate[field]:
                differences.append({"frame": index, "case": native["case"], "field": field,
                                    "reference": native[field], "candidate": candidate[field]})
        timings.append({
            "frame": index, "case": native["case"], "direction": native["direction"],
            **{phase: {"reference": native.get(phase), "candidate": candidate.get(phase)}
               for phase in TIMED_PHASES},
        })
    return differences, timings


def compare_received(actions, received):
    """Check CPU observations and their provenance against selected receive actions."""
    expected = [(index, action.split()) for index, action in enumerate(actions)
                if action.split()[1] == "R"]
    differences = []
    if len(received) != len(expected):
        differences.append({"case": "coverage", "field": "received record count",
                            "reference": len(expected), "candidate": len(received)})
    for (index, action), row in zip(expected, received):
        fields = {
            "action": index, "case": action[0],
            "expected": int(action[2]), "actual": int(action[2]),
            "expected_eoi": int(action[4]), "actual_eoi": int(action[4]),
        }
        for field, value in fields.items():
            candidate = row.get(field)
            if field != "case":
                try:
                    candidate = int(candidate)
                except (TypeError, ValueError):
                    pass
            if candidate != value:
                differences.append({"action": index, "case": action[0], "field": field,
                                    "reference": value, "candidate": candidate})
    return differences


def ordinary_file_prefix(actions):
    """Keep the captured prefix through readback/UNTALK, excluding the later UJ command."""
    last = max(index for index, action in enumerate(actions)
               if action.split()[:2] == ["readback", "R"])
    suffix = [action.split()[1:] for action in actions[last + 1:last + 3]]
    if suffix != [["H", "95", "1", "0"], ["A", "0", "0", "0"]]:
        raise ValueError("ordinary-file reference must end with captured UNTALK and ATN release")
    return actions[:last + 3]


def main():
    """Verify identical inputs, run a bounded replay, and classify gaps without masking them."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--replay-tool", type=Path,
                        default=Path("build/system/board/econopet_drive_reference"))
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--read-only", action="store_true",
                           help="compare the exact prefix before the first write transaction")
    selection.add_argument("--ordinary-files", action="store_true",
                           help="include write/close/readback, excluding the later UJ command")
    args = parser.parse_args()
    reference = args.reference_dir.resolve()
    original = json.loads((reference / "result.json").read_text(encoding="ascii"))
    if not original.get("wire"):
        raise ValueError("requires complete composed-net original-ROM observations")
    extension = MODELS[original["model"]][0]
    initial = reference / ("initial." + extension)
    if sha256(initial) != original["media"]["initial_sha256"]:
        raise ValueError("reference input image identity changed")
    for name in ("ieee-wire.csv", "replay.txt"):
        if sha256(reference / name) != original["artifacts"][name]:
            raise ValueError("reference transaction identity changed")
    output = args.output_dir.resolve()
    check_output_directory(output)
    actions = (reference / "replay.txt").read_text(encoding="ascii").splitlines()[1:]
    selected_actions = actions
    replay_script = reference / "replay.txt"
    if args.read_only:
        # Keep the identical ordered prefix, not a synthesized or normalized scenario.
        first_write = next(index for index, action in enumerate(actions)
                           if action.split()[0] == "write")
        selected_actions = actions[:first_write]
        replay_script = output / "read-only-replay.txt"
        replay_script.write_text("ieee-replay-v1\n" + "\n".join(selected_actions) + "\n",
                                 encoding="ascii")
    elif args.ordinary_files:
        selected_actions = ordinary_file_prefix(actions)
        replay_script = output / "ordinary-files-replay.txt"
        replay_script.write_text("ieee-replay-v1\n" + "\n".join(selected_actions) + "\n",
                                 encoding="ascii")
    tool = args.replay_tool.resolve()
    command = [str(tool), str(initial), str(replay_script), str(output / "candidate")]
    with (output / "replay.log").open("w", encoding="ascii") as log:
        execution = subprocess.run(
            command,
            cwd=tool.parent, stdout=log, stderr=subprocess.STDOUT, timeout=900, check=False)
    if execution.returncode not in (0, 2):
        raise ValueError(f"replay failed outside a retained transaction prefix (see {output}/replay.log)")
    candidate = output / "candidate"
    coverage = (candidate / "coverage.txt").read_text(encoding="ascii")
    completed = int(coverage.splitlines()[0].removeprefix("completed="))
    failure_case = None if completed == len(selected_actions) else selected_actions[completed].split()[0]
    observed = decode_wire(candidate / "ieee-wire.csv")
    observed["domain"] = "fpga-virtual-composed"
    all_native_frames = original["wire"]["frames"]
    selected_frames = sum(action.split()[1] in ("H", "R") for action in selected_actions)
    native_frames = all_native_frames[:selected_frames]
    differences, timings = compare_frames(native_frames, observed["frames"])
    with (candidate / "received.csv").open(encoding="ascii") as stream:
        received = list(csv.DictReader(stream))
    received_differences = compare_received(selected_actions, received)
    unexplained = [*differences, *received_differences]
    expected_completed_frames = sum(action.split()[1] in ("H", "R")
                                    for action in selected_actions[:completed])
    if (len(observed["frames"]) < expected_completed_frames
            or (not failure_case and len(observed["frames"]) != len(native_frames))):
        unexplained.append({"case": "coverage", "field": "missing/extra accepted wire frames"})
    if failure_case:
        unexplained.append({"case": failure_case, "field": "incomplete replay"})
    final = candidate / ("final." + extension)
    candidate_bytes = final.read_bytes()
    before = initial.read_bytes()
    if len(candidate_bytes) != len(before) or sha256(initial) != original["media"]["initial_sha256"]:
        raise ValueError("replay changed geometry or the immutable input")
    changed = [offset // 256 for offset in range(0, len(before), 256)
               if before[offset:offset + 256] != candidate_bytes[offset:offset + 256]]
    expected_media = original["media"]["initial_sha256" if args.read_only else "final_sha256"]
    if args.read_only and changed:
        unexplained.append({"case": "read-only", "field": "unexpected media mutation"})
    if not failure_case and not differences and sha256(final) != expected_media:
        unexplained.append({"case": "write", "field": "media differs despite matching transactions"})
    report = {
        "schema": 1, "model": original["model"], "engine": "production-firmware",
        "read_only_prefix": args.read_only, "selected_replay_sha256": sha256(replay_script),
        "ordinary_files_prefix": args.ordinary_files,
        "reference_sha256": sha256(reference / "result.json"),
        "replay_tool_sha256": sha256(tool), "input_sha256": sha256(initial),
        "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "dirty_worktree": subprocess.check_output(["git", "status", "--porcelain"], text=True),
        "stimulus": {"requested_actions": len(selected_actions), "completed_actions": completed,
                     "excluded_suffix_actions": len(actions) - len(selected_actions),
                     "failure_case": failure_case, "coverage": coverage},
        "received": received, "received_differences": received_differences, "wire": observed,
        "differences": differences, "timings": timings,
        "unexplained_differences": unexplained,
        "not_compared_frames": len(all_native_frames) - len(observed["frames"]),
        "not_compared_cases": sorted({row["case"] for row in all_native_frames[len(observed["frames"]):]}),
        "media": {
            "reference_sha256": expected_media,
            "candidate_sha256": sha256(final),
            "equal": sha256(final) == expected_media,
            "candidate_changed_sectors": changed,
        },
        "limitations": [
            *(["write/readback/reset suffix is explicitly excluded from this read-only prefix"]
              if args.read_only else []),
            *(["UJ/reset suffix is explicitly excluded from the ordinary-file prefix"]
              if args.ordinary_files else []),
            "missing suffix observations are not successful comparisons",
            "64 MHz completed-cycle samples do not resolve simulator delta-cycle edges",
            "candidate nets are the FPGA virtual bus, not separately delayed fitted GPIO outputs",
            "native BASIC and mailbox controllers have different software pacing",
            "host filesystem calls omit physical SD/RP2040 execution latency",
            "phase times are diagnostic simulation comparisons, not physical latency parity",
            "cold boot IFC is captured by the reference but is not a replayed byte transaction",
        ],
    }
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
    print(f'{original["model"]}: {len(observed["frames"])} accepted frames, '
          f"{len(unexplained)} unexplained differences, stopped case={failure_case}")
    if unexplained or observed["issues"] or observed["aborted"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
