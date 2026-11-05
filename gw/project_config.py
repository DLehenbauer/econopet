#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Fingerprint Efinity configuration independently of XML serialization."""

import hashlib
import json
import sys
import xml.etree.ElementTree as ET


# efx_run.py update_project_file() writes these and prune_project_file() removes
# the legacy attributes. Neither group is an input to synthesis.
RUN_METADATA = {
    "last_change", "sw_version", "last_run_state", "last_run_flow",
    "config_result_in_sync", "design_ood", "place_ood", "route_ood",
    "last_change_date", "location", "last_run_tool",
}


def configuration(element):
    """Retain ordered elements and attribute values, but not XML presentation."""
    return (
        element.tag,
        sorted(element.attrib.items()),
        element.text if element.text and element.text.strip() else "",
        [(configuration(child),
          child.tail if child.tail and child.tail.strip() else "")
         for child in element],
    )


def main():
    """Print a stable digest without changing the source project."""
    root = ET.parse(sys.argv[1]).getroot()
    for attribute in RUN_METADATA:
        root.attrib.pop(attribute, None)
    encoded = json.dumps(configuration(root), ensure_ascii=True).encode("ascii")
    print(hashlib.sha256(encoded).hexdigest())


if __name__ == "__main__":
    main()
