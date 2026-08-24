#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("check-timing.py")
HEADER = "Launch Clock Capture Clock Constraint (ns) Slack (ns) Edge"
REPORT = f"""\
Efinix Static Timing Analysis Report
---------- 2. Clock Relationship Summary (begin) ----------

Setup (Max) Clock Relationship
{HEADER}
sys_clock_i sys_clock_i 15.625 {{setup_slack}} (R-R)

Hold (Min) Clock Relationship
{HEADER}
sys_clock_i sys_clock_i 0.000 {{hold_slack}} (R-R)

NOTE: Values are in nanoseconds.

---------- Clock Relationship Summary (end) ---------------
"""


class CheckTimingTests(unittest.TestCase):
    def run_check(self, report: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "EconoPET.timing.rpt"
            report_path.write_text(report, encoding="utf-8")
            return subprocess.run(
                [sys.executable, SCRIPT, report_path],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_accepts_nonnegative_setup_and_hold_slack(self) -> None:
        result = self.run_check(REPORT.format(setup_slack="1.250", hold_slack="0.125"))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Gateware passed timing closure", result.stdout)

    def test_rejects_negative_slack(self) -> None:
        result = self.run_check(REPORT.format(setup_slack="-0.001", hold_slack="0.125"))

        self.assertEqual(result.returncode, 1)
        self.assertIn("gateware failed timing closure", result.stderr)

    def test_rejects_changed_column_header(self) -> None:
        report = REPORT.format(setup_slack="1.250", hold_slack="0.125")
        result = self.run_check(report.replace("Slack (ns)", "Margin (ns)"))

        self.assertEqual(result.returncode, 1)
        self.assertIn("failed to parse", result.stderr)

    def test_rejects_malformed_relationship(self) -> None:
        result = self.run_check(REPORT.format(setup_slack="unknown", hold_slack="0.125"))

        self.assertEqual(result.returncode, 1)
        self.assertIn("failed to parse", result.stderr)

    def test_rejects_non_finite_slack(self) -> None:
        result = self.run_check(REPORT.format(setup_slack="nan", hold_slack="0.125"))

        self.assertEqual(result.returncode, 1)
        self.assertIn("failed to parse", result.stderr)

    def test_rejects_incomplete_summary(self) -> None:
        report = REPORT.format(setup_slack="1.250", hold_slack="0.125")
        result = self.run_check(report.replace(
            "---------- Clock Relationship Summary (end) ---------------",
            "",
        ))

        self.assertEqual(result.returncode, 1)
        self.assertIn("failed to parse", result.stderr)


if __name__ == "__main__":
    unittest.main()
