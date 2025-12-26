#!/usr/bin/env python3
"""
CC0 1.0 Universal
To the extent possible under law, the author(s) have dedicated all copyright
and related and neighboring rights to this file to the public domain worldwide.
This file is distributed without any warranty.

Filters bom.csv and positions.csv for JLCPCB SMT assembly.

Removes rows from bom.csv that have an empty LCSC Part # field (hand-assembled
parts), then removes the corresponding designators from positions.csv.

Usage:
    python filter_for_jlcpcb.py

Outputs:
    bom.filtered.csv       - BOM with only LCSC-sourced parts
    positions.filtered.csv - Positions with only LCSC-sourced parts
"""

import csv
from pathlib import Path


def parse_designators(designator_field: str) -> set[str]:
    """Parse a designator field like 'C1, C2, C3' into a set of designators."""
    return {d.strip() for d in designator_field.split(",") if d.strip()}


def main() -> None:
    script_dir = Path(__file__).parent
    bom_in = script_dir / "bom.csv"
    bom_out = script_dir / "jlcpcb-rev-a-bom.csv"
    pos_in = script_dir / "positions.csv"
    pos_out = script_dir / "jlcpcb-rev-a-cpl.csv"

    # --- Pass 1: Filter BOM and collect designators to remove ---
    designators_to_remove: set[str] = set()

    # Use utf-8-sig to strip the BOM if present
    with bom_in.open(newline="", encoding="utf-8-sig") as src, \
         bom_out.open("w", newline="", encoding="utf-8") as dst:
        reader = csv.DictReader(src)
        writer = csv.DictWriter(dst, fieldnames=reader.fieldnames)
        writer.writeheader()

        for row in reader:
            lcsc = row.get("LCSC Part #", "").strip()
            if not lcsc:
                # Record all designators from this row for removal
                designators_to_remove |= parse_designators(row.get("Designator", ""))
                continue
            writer.writerow(row)

    print(f"BOM: removed {len(designators_to_remove)} designator(s) lacking LCSC Part #")
    if designators_to_remove:
        print(f"  Removed: {', '.join(sorted(designators_to_remove))}")

    # --- Pass 2: Filter positions based on collected designators ---
    removed_count = 0

    # Use utf-8-sig to strip the BOM if present
    with pos_in.open(newline="", encoding="utf-8-sig") as src, \
         pos_out.open("w", newline="", encoding="utf-8") as dst:
        reader = csv.DictReader(src)
        writer = csv.DictWriter(dst, fieldnames=reader.fieldnames)
        writer.writeheader()

        for row in reader:
            if row.get("Designator", "").strip() in designators_to_remove:
                removed_count += 1
                continue
            writer.writerow(row)

    print(f"Positions: removed {removed_count} row(s)")
    print(f"\nOutput files:\n  {bom_out}\n  {pos_out}")


if __name__ == "__main__":
    main()
