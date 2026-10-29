#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
r"""Reconstruct drive ROM assembly from cbmsrc and binaries, then verify its bytes.

Coordinate eight checks for the approved Commodore 4040 revision-2 and
full-size 8050/8250 DOS 2.7 ROMs. Use pinned cbmsrc source where available,
translating assembler syntax and applying explicit source adaptations.
Reconstruct selected controller and embedded payloads by disassembling the
original ROM binaries with manifest-declared code/data boundaries and entries.

Assemble the generated source and compare it byte by byte with the approved
original binaries. Source-based checks exclude declared checksum placeholders
and report uncovered regions, rather than requiring whole-ROM equality.
Binary payload reconstructions require exact equality of every selected byte.
The older, unmodified 4040 source must reproduce its recorded revision mismatch
as a negative control, not pass as equivalent to the approved revision.

Keep reconstructed source, assembled images, reports, logs, and summary.json
under --output-dir. These are static byte checks, not drive execution tests.
Inputs must already exist (verify.sh acquires them and invokes this runner).

Example from the repository root, using acquired inputs and a new output:

    python3 tools/drive-roms/verify.py \
        --source-root .tmp/ai/cbmsrc \
        --rom-dir .tmp/ai/drive-roms/download \
        --output-dir .tmp/ai/drive-roms/out/manual-checks
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import drive_rom_verify


SOURCE_MAPPINGS = {
    "4040-revision2": "DOS_4040",
    "8050-8250-mpi-controller": "CONTROLLER_8250",
}
BINARY_MAPPINGS = (
    "4040-controller",
    "4040-formatter",
    "8050-8250-formatter",
    "8050-8250-initialization",
)
REQUIRED_TOOLS = ("acme", "da65", "ca65", "ld65")


def digest(path):
    """Identify an input or tool without including its contents in the summary."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cases(source, manifest):
    """Declare every required check and its independently recorded expectations."""
    result = []
    for family in ("8250", "4040"):
        baseline = manifest["source_verification"]["DOS_" + family]
        expected = {
            key: baseline[key]
            for key in ("emitted_bytes", "compared_bytes", "matching_bytes")
        }
        expected.update({
            "status": "mismatch" if family == "4040" else "emitted-bytes-match",
            "mismatch_count": baseline["mismatch_count"] if family == "4040" else 0,
            "source_commit": manifest["source"]["commit"],
            "source_digest": drive_rom_verify.FAMILIES[family]["digest"],
        })
        if family == "4040" and expected["mismatch_count"] <= 0:
            raise ValueError("4040 negative control requires recorded mismatches")
        result.append((
            family + "-source", "drive_rom_verify.py",
            ["--family", family, "--source-dir", str(source / ("DOS_" + family))],
            "comparison.json", 1 if family == "4040" else 0, expected,
        ))
    for name, directory in SOURCE_MAPPINGS.items():
        recipe = manifest["source_mappings"][name]
        result.append((
            name, "drive_rom_map.py",
            ["--mapping", name, "--source-dir", str(source / directory)],
            "mapping.json", 0, {
                "mismatch_count": 0,
                "source_commit": manifest["source"]["commit"],
                "source_digest": recipe["source_digest"],
            },
        ))
    for name in BINARY_MAPPINGS:
        recipe = manifest["binary_mappings"][name]
        result.append((
            name, "drive_rom_map.py", ["--binary-map", name],
            "mapping.json", 0, {
                "payload_bytes": recipe["size"],
                "payload_sha256": recipe["payload_sha256"],
                "roundtrip": "all payload bytes match",
            },
        ))
    return result


def run_case(root, common, case):
    """Retain a child's log and reject unexpected exits or incomplete reports."""
    name, tool, arguments, report_name, expected_exit, expected = case
    directory = root / name
    command = [
        sys.executable, str(Path(__file__).with_name(tool)), *arguments,
        *common, "--output-dir", str(directory),
    ]
    with (root / (name + ".log")).open("x", encoding="ascii") as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
    report_path = directory / report_name
    error = None
    try:
        report = json.loads(report_path.read_text(encoding="ascii"))
        passed = (
            process.returncode == expected_exit
            and all(report.get(key) == value for key, value in expected.items())
        )
        if "mismatch_count" in expected:
            passed = passed and (
                report.get("compared_bytes", 0) > 0
                and report.get("matching_bytes", -1) + report.get("mismatch_count", -1)
                == report["compared_bytes"]
            )
    except (OSError, ValueError, TypeError, AttributeError, KeyError) as failure:
        passed = False
        error = str(failure)
    return {
        "case": name, "passed": passed, "command": command,
        "exit_code": process.returncode, "expected_exit_code": expected_exit,
        "expected": expected, "report": str(report_path.relative_to(root)),
        "report_sha256": digest(report_path) if report_path.is_file() else None,
        "error": error,
    }


def main(argv=None):
    """Validate prerequisites before creating fresh private evidence and a summary."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True,
                        help="pinned cbmsrc root containing DOS_4040, DOS_8250 and CONTROLLER_8250")
    parser.add_argument("--rom-dir", type=Path, required=True, help="external chip .bin files")
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("drive-emulation-roms.json"))
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new private output directory, never publish its contents")
    args = parser.parse_args(argv)
    source, roms, output = (
        path.resolve() for path in (args.source_root, args.rom_dir, args.output_dir)
    )
    for directory in (source, roms, *(source / name for name in
                                    ("DOS_4040", "DOS_8250", "CONTROLLER_8250"))):
        if not directory.is_dir():
            raise ValueError(f"missing input directory: {directory}")
    if any(output == directory or directory in output.parents for directory in (source, roms)):
        raise ValueError("output must be outside the supplied input directories")
    missing = [tool for tool in REQUIRED_TOOLS if shutil.which(tool) is None]
    if missing:
        raise ValueError("missing required tools: " + ", ".join(missing))
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    if manifest["source"]["commit"] != drive_rom_verify.SOURCE_COMMIT:
        raise ValueError("manifest does not identify the supported source commit")
    plan = cases(source, manifest)

    # Never overwrite prior evidence or place derived files inside supplied inputs.
    output.mkdir(parents=True, exist_ok=False)
    common = ["--rom-dir", str(roms), "--manifest", str(manifest_path)]
    results = []
    for case in plan:
        result = run_case(output, common, case)
        results.append(result)
        label = "PASS" if result["passed"] else "FAIL"
        suffix = " (expected mismatch)" if result["expected_exit_code"] == 1 else ""
        print(f"{label} {result['case']}{suffix}")

    # Bind the summary to exact tools and retain failures instead of hiding them.
    summary = {
        "passed": all(result["passed"] for result in results),
        "manifest_sha256": digest(manifest_path),
        "tools_sha256": {
            name: digest(Path(__file__).with_name(name))
            for name in ("verify.py", "drive_rom_verify.py", "drive_rom_map.py")
        },
        "results": results,
        "limits": "static correspondence only, not drive execution or hardware acceptance",
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="ascii"
    )
    print(f"{sum(result['passed'] for result in results)}/{len(results)} checks passed")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"drive ROM checks failed: {error}", file=sys.stderr)
        sys.exit(2)
