# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

import collections
import csv
import pathlib
import tempfile


TARGET_DESIGNATORS = frozenset(
    "J21,J20,J23,J22,LS1,J27,SW3,SW5,J1,J2,J10,J3,J4,J12,J5,J11,J6,"
    "J14,J7,J13,J8,J16,J9,J15,J18,C111,J17,C110,J19,U11,U13,U12,U1".split(",")
)


def main() -> None:
    positions_path = pathlib.Path(__file__).with_name("positions.csv")

    with positions_path.open(newline="", encoding="utf-8-sig") as positions_file:
        reader = csv.DictReader(positions_file)
        if reader.fieldnames is None or "Designator" not in reader.fieldnames:
            raise ValueError("positions.csv has no Designator column")
        rows = list(reader)

    counts = collections.Counter(row["Designator"] for row in rows)
    unexpected_counts = {
        designator: counts[designator]
        for designator in TARGET_DESIGNATORS
        if counts[designator] != 1
    }
    if unexpected_counts:
        raise ValueError(f"target designators must occur exactly once: {unexpected_counts}")

    remaining_rows = [
        row for row in rows if row["Designator"] not in TARGET_DESIGNATORS
    ]

    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            dir=positions_path.parent,
            encoding="utf-8-sig",
            newline="",
            delete=False,
        ) as temporary_file:
            temporary_path = pathlib.Path(temporary_file.name)
            writer = csv.DictWriter(
                temporary_file,
                fieldnames=reader.fieldnames,
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(remaining_rows)
        temporary_path.replace(positions_path)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()

    print(f"Removed {len(rows) - len(remaining_rows)} rows from {positions_path.name}")


if __name__ == "__main__":
    main()