# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

import csv
from pathlib import Path


def read_csv(path):
    with path.open(newline="", encoding="utf-8-sig") as csv_file:
        reader = csv.DictReader(csv_file)
        if reader.fieldnames is None:
            raise ValueError(f"{path.name} has no header")
        return reader.fieldnames, list(reader)


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8-sig") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


directory = Path(__file__).parent
bom_path = directory / "bom.csv"
positions_path = directory / "positions.csv"

bom_fields, bom_rows = read_csv(bom_path)
position_fields, position_rows = read_csv(positions_path)
if "LCSC Part #" not in bom_fields or "Designator" not in bom_fields:
    raise ValueError("bom.csv is missing a required column")
if "Designator" not in position_fields:
    raise ValueError("positions.csv is missing the Designator column")

filtered_bom = [row for row in bom_rows if row["LCSC Part #"].strip()]
bom_designators = {
    designator.strip()
    for row in filtered_bom
    for designator in row["Designator"].split(",")
}
filtered_positions = [
    row for row in position_rows if row["Designator"].strip() in bom_designators
]

write_csv(bom_path, bom_fields, filtered_bom)
write_csv(positions_path, position_fields, filtered_positions)
print(f"Removed {len(bom_rows) - len(filtered_bom)} BOM rows")
print(f"Removed {len(position_rows) - len(filtered_positions)} position rows")