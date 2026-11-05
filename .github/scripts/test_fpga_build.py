#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check FPGA invalidation with a deterministic stand-in for Efinity."""

import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NAMESPACE = "http://www.efinixinc.com/enf_proj"


class FpgaBuildTests(unittest.TestCase):
    """Exercise the production bitstream dependency graph without synthesis."""

    def setUp(self):
        """Create a small project whose compiler updates Efinity run metadata."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / "gw"
        project = source / "EconoPET"
        simulation = project / "sim"
        simulation.mkdir(parents=True)
        shutil.copyfile(ROOT / "gw/EconoPET/sim/CMakeLists.txt",
                        simulation / "CMakeLists.txt")
        shutil.copyfile(ROOT / "gw/CMakeLists.txt", source / "CMakeLists.txt")
        helper = ROOT / "gw/project_config.py"
        shutil.copyfile(helper, source / helper.name)
        self.project_file = project / "EconoPET.xml"
        ET.register_namespace("efx", NAMESPACE)
        tree = ET.Element(f"{{{NAMESPACE}}}project", name="EconoPET", last_change="0")
        synthesis = ET.SubElement(tree, f"{{{NAMESPACE}}}synthesis")
        ET.SubElement(synthesis, f"{{{NAMESPACE}}}param", name="mode", value="speed")
        self.project_file.write_bytes(ET.tostring(tree))
        external = project / "external/core/rtl"
        external.mkdir(parents=True)
        self.external_file = external / "core.v"
        self.external_file.write_text("original")
        compiler = source / "efx.sh"
        compiler.write_text("""#!/usr/bin/env python3
import pathlib
import sys
import xml.etree.ElementTree as ET

assert sys.argv[1:3] == ["--flow", "compile"], sys.argv
output = pathlib.Path(sys.argv[sys.argv.index("--output_dir") + 1])
output.mkdir(parents=True, exist_ok=True)
pathlib.Path(sys.argv[sys.argv.index("--work_dir") + 1]).mkdir(parents=True, exist_ok=True)
ET.register_namespace("efx", "http://www.efinixinc.com/enf_proj")
project = pathlib.Path("EconoPET/EconoPET.xml")
tree = ET.parse(project)
root = tree.getroot()
bitstream = output / "EconoPET.hex.bin"
count = int(bitstream.read_text()) + 1 if bitstream.exists() else 1
root.set("last_change", str(count))
tree.write(project)
bitstream.write_text(str(count))
""")
        compiler.chmod(0o755)
        self.build = self.root / "build"
        self.run_command(
            "cmake", "-S", str(source), "-B", str(self.build), "-G", "Ninja",
            f"-DECONOPET_ROMS_DIR={self.root / 'roms'}")
        self.build_command = ("cmake", "--build", str(self.build),
                              "--target", "fpga_bitstream")
        self.run_command(*self.build_command)

    def run_command(self, *command):
        """Run CMake and retain diagnostics for dependency-graph failures."""
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def compile_count(self):
        """Observe actual compiler executions through the fake bitstream."""
        return int((self.build / "outflow/EconoPET.hex.bin").read_text())

    def test_project_options_rebuild_but_run_metadata_does_not(self):
        """Recompile option edits without looping on tool-written metadata."""
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 1)
        tree = ET.parse(self.project_file)
        tree.find(f".//{{{NAMESPACE}}}param").set("value", "area")
        tree.write(self.project_file)
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 2)
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 2)

    def test_nested_external_sources_invalidate_bitstream(self):
        """Track edits and newly added sources below external core directories."""
        self.external_file.write_text("changed")
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 2)
        self.external_file.with_name("new.v").write_text("new source")
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 3)

    def test_xml_serialization_and_cli_metadata_preserve_bitstream(self):
        """Ignore XML formatting and the exact metadata updated/pruned by efx_run."""
        self.run_command(*self.build_command)
        fingerprint = self.build / "project-config.sha256"
        timestamp = fingerprint.stat().st_mtime_ns
        tree = ET.parse(self.project_file)
        root = tree.getroot()
        for attribute in ("last_change", "sw_version", "last_run_state",
                          "last_run_flow", "config_result_in_sync",
                          "design_ood", "place_ood", "route_ood",
                          "last_change_date", "location", "last_run_tool"):
            root.set(attribute, "tool metadata")
        # Reorder attributes, change prefixes/quotes and expand empty elements.
        root.attrib = dict(reversed(list(root.attrib.items())))
        ET.indent(tree, space="\t")
        ET.register_namespace("other", NAMESPACE)
        xml = ET.tostring(root, encoding="unicode", short_empty_elements=False)
        self.project_file.write_text(
            '<?xml version="1.0"?>\r\n<!-- command-line rewrite -->\r\n'
            + xml.replace('"', "'").replace("\n", "\r\n"))
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 1)
        self.assertEqual(fingerprint.stat().st_mtime_ns, timestamp)

        # CLI pruning removes legacy status attributes before its next write.
        tree = ET.parse(self.project_file)
        for attribute in ("last_change_date", "location", "last_run_tool"):
            tree.getroot().attrib.pop(attribute)
        ET.indent(tree, space="  ")
        tree.write(self.project_file, xml_declaration=True, encoding="utf-8")
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 1)
        self.assertEqual(fingerprint.stat().st_mtime_ns, timestamp)

        # Significant attribute whitespace is data, not XML indentation.
        tree.find(f".//{{{NAMESPACE}}}param").set("value", "speed ")
        tree.write(self.project_file)
        self.run_command(*self.build_command)
        self.assertEqual(self.compile_count(), 2)


if __name__ == "__main__":
    unittest.main()
