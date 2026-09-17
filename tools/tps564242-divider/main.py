#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
"""Rank TPS564242 feedback-divider resistor networks for a target output."""

import argparse
import itertools
import math

from rich import box
from rich.console import Console
from rich.table import Table

RESISTORS_0402 = (
    ("0", 0.0), ("10", 10.0), ("22", 22.0), ("33", 33.0), ("49.9", 49.9),
    ("100", 100.0), ("120", 120.0), ("200", 200.0), ("220", 220.0),
    ("330", 330.0), ("470", 470.0), ("510", 510.0), ("1k", 1_000.0),
    ("1.5k", 1_500.0), ("2k", 2_000.0), ("2.2k", 2_200.0),
    ("3.3k", 3_300.0), ("4.7k", 4_700.0), ("5.1k", 5_100.0),
    ("10k", 10_000.0), ("12k", 12_000.0), ("15k", 15_000.0),
    ("20k", 20_000.0), ("22k", 22_000.0), ("33k", 33_000.0),
    ("47k", 47_000.0), ("51k", 51_000.0), ("100k", 100_000.0),
    ("200k", 200_000.0), ("1M", 1_000_000.0),
)

RESISTORS_0402_OR_0603 = (
    ("0", 0.0), ("10", 10.0), ("22", 22.0), ("33", 33.0), ("49.9", 49.9),
    ("100", 100.0), ("120", 120.0), ("200", 200.0), ("220", 220.0),
    ("330", 330.0), ("390", 390.0), ("470", 470.0), ("510", 510.0),
    ("560", 560.0), ("680", 680.0), ("820", 820.0), ("1k", 1_000.0),
    ("1.2k", 1_200.0), ("1.5k", 1_500.0), ("1.8k", 1_800.0),
    ("2k", 2_000.0), ("2.2k", 2_200.0), ("2.4k", 2_400.0),
    ("2.7k", 2_700.0), ("3k", 3_000.0), ("3.3k", 3_300.0),
    ("3.6k", 3_600.0), ("3.9k", 3_900.0), ("4.7k", 4_700.0),
    ("4.99k", 4_990.0), ("5.1k", 5_100.0), ("5.6k", 5_600.0),
    ("6.2k", 6_200.0), ("6.8k", 6_800.0), ("7.5k", 7_500.0),
    ("8.2k", 8_200.0), ("9.1k", 9_100.0), ("10k", 10_000.0),
    ("12k", 12_000.0), ("13k", 13_000.0), ("15k", 15_000.0),
    ("18k", 18_000.0), ("20k", 20_000.0), ("22k", 22_000.0),
    ("24k", 24_000.0), ("27k", 27_000.0), ("30k", 30_000.0),
    ("33k", 33_000.0), ("39k", 39_000.0), ("47k", 47_000.0),
    ("49.9k", 49_900.0), ("51k", 51_000.0), ("56k", 56_000.0),
    ("68k", 68_000.0), ("75k", 75_000.0), ("82k", 82_000.0),
    ("100k", 100_000.0), ("120k", 120_000.0), ("150k", 150_000.0),
    ("200k", 200_000.0), ("220k", 220_000.0), ("300k", 300_000.0),
    ("330k", 330_000.0), ("470k", 470_000.0), ("510k", 510_000.0),
    #("1M", 1_000_000.0), ("2M", 2_000_000.0), ("10M", 10_000_000.0),
)

#RESISTORS = RESISTORS_0402
RESISTORS = RESISTORS_0402_OR_0603

RESISTOR_TOLERANCE = 0.01
MIN_DIVIDER_CURRENT = 20e-6
MAX_DIVIDER_CURRENT = 100e-6


def equivalent_resistance(parts):
    if any(value == 0 for _, value in parts):
        return 0.0
    return 1.0 / sum(1.0 / value for _, value in parts)


def network_tolerance(parts):
    if any(value == 0 for _, value in parts):
        return 0.0
    return RESISTOR_TOLERANCE


def network_statistical_tolerance(parts):
    if any(value == 0 for _, value in parts):
        return 0.0
    conductances = [1.0 / value for _, value in parts]
    total_conductance = sum(conductances)
    return RESISTOR_TOLERANCE * math.sqrt(sum(
        (conductance / total_conductance) ** 2
        for conductance in conductances))


def format_resistance(value):
    if value >= 1_000_000:
        return f"{value / 1_000_000:.6g}M"
    if value >= 1_000:
        return f"{value / 1_000:.6g}k"
    return f"{value:.6g}"


def format_current(current):
    if current >= 0.001:
        return f"{current * 1_000:.6g} mA"
    if current >= 0.000001:
        return f"{current * 1_000_000:.6g} uA"
    return f"{current * 1_000_000_000:.6g} nA"


def format_power(power):
    if power >= 0.001:
        return f"{power * 1_000:.6g} mW"
    return f"{power * 1_000_000:.6g} uW"


def format_network(parts):
    components = " || ".join(name for name, _ in parts)
    if len(parts) == 1:
        return components
    return f"{format_resistance(equivalent_resistance(parts))} = {components}"


def maximum_part_power(parts, voltage):
    return max((voltage ** 2 / value for _, value in parts if value != 0), default=0.0)


def networks(max_parallel, allow_zero):
    choices = RESISTORS if allow_zero else RESISTORS[1:]
    for count in range(1, max_parallel + 1):
        for parts in itertools.combinations_with_replacement(choices, count):
            yield parts, equivalent_resistance(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vmin", type=float,
                        help="minimum output voltage in volts")
    parser.add_argument("--vmax", type=float,
                        help="maximum output voltage in volts")
    parser.add_argument("--vout", type=float, required=True,
                        help="target output voltage in volts")
    parser.add_argument("--max-parallel", type=int, default=2,
                        help="maximum resistors in each parallel network (default: 2)")
    args = parser.parse_args()
    for name in ("vmin", "vmax", "vout"):
        value = getattr(args, name)
        if value is not None and value <= 0:
            parser.error(f"--{name} must be positive")
    if args.vmin is not None and args.vmax is not None and args.vmin > args.vmax:
        parser.error("--vmin must be less than or equal to --vmax")
    if args.vmin is not None and args.vout is not None and args.vmin > args.vout:
        parser.error("--vmin must be less than or equal to --vout")
    if args.vmax is not None and args.vout is not None and args.vmax < args.vout:
        parser.error("--vmax must be greater than or equal to --vout")
    if args.max_parallel < 1:
        parser.error("--max-parallel must be at least 1")

    r1_networks = list(networks(args.max_parallel, allow_zero=True))
    r2_networks = list(networks(args.max_parallel, allow_zero=False))
    candidates = []
    for r1_parts, r1 in r1_networks:
        for r2_parts, r2 in r2_networks:
            vout = 0.6 * (1.0 + r1 / r2)
            r1_tolerance = network_tolerance(r1_parts)
            r2_tolerance = network_tolerance(r2_parts)
            vmin = 0.6 * (1.0 + r1 * (1.0 - r1_tolerance) /
                           (r2 * (1.0 + r2_tolerance)))
            vmax = 0.6 * (1.0 + r1 * (1.0 + r1_tolerance) /
                           (r2 * (1.0 - r2_tolerance)))
            current = vout / (r1 + r2)
            statistical_spread = (vout - 0.6) * math.hypot(
                network_statistical_tolerance(r1_parts),
                network_statistical_tolerance(r2_parts))
            if (MIN_DIVIDER_CURRENT <= current <= MAX_DIVIDER_CURRENT and
                    (args.vmin is None or vmin >= args.vmin) and
                    (args.vmax is None or vmax <= args.vmax)):
                target_offset = vout - args.vout
                statistical_min_error = target_offset - statistical_spread
                statistical_max_error = target_offset + statistical_spread
                statistical_error = max(abs(statistical_min_error),
                                        abs(statistical_max_error))
                candidates.append((statistical_error,
                                   abs(target_offset),
                                   len(r1_parts) + len(r2_parts),
                                   vout, vmin, vmax, current,
                                   statistical_min_error,
                                   statistical_max_error,
                                   max(maximum_part_power(r1_parts, vout - 0.6),
                                       maximum_part_power(r2_parts, 0.6)), r1_parts, r2_parts))

    candidates.sort(key=lambda candidate: candidate[:4])
    criteria = []
    for name in ("vout", "vmin", "vmax"):
        value = getattr(args, name)
        if value is not None:
            criteria.append(f"{name.upper()} {value:.6g} V")
    table = Table(title=f"TPS564242 divider candidates for {', '.join(criteria)}",
              caption="1% resistor tolerance: worst-case VMIN/VMAX, "
                  "independent RSS STAT_ERR (-/+ mV from VOUT); I_DIV: 20-100 uA",
                  box=box.ASCII, header_style="bold cyan", collapse_padding=True)
    table.add_column("Rank", justify="right", min_width=4, no_wrap=True)
    table.add_column("VOUT (V)", justify="right", no_wrap=True)
    table.add_column("VMIN (V)", justify="right", no_wrap=True)
    table.add_column("VMAX (V)", justify="right", no_wrap=True)
    table.add_column("STAT_ERR", justify="right", no_wrap=True)
    table.add_column("I_DIV", justify="right", no_wrap=True)
    table.add_column("P_MAX", justify="right", no_wrap=True)
    table.add_column("R1", max_width=23, overflow="fold")
    table.add_column("R2", max_width=23, overflow="fold")
    for rank, (_, _, _, vout, vmin, vmax, current, statistical_min_error,
               statistical_max_error, maximum_power, r1_parts,
               r2_parts) in enumerate(candidates[:20], 1):
        table.add_row(str(rank), f"{vout:.6f}", f"{vmin:.6f}", f"{vmax:.6f}",
                      f"{statistical_min_error * 1_000:+.4g}/"
                      f"{statistical_max_error * 1_000:+.4g}",
                      format_current(current), format_power(maximum_power),
                      format_network(r1_parts), format_network(r2_parts))
    Console().print(table)


if __name__ == "__main__":
    main()