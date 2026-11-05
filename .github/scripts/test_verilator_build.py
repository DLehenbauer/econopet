# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Check native model invalidation when SystemVerilog dependencies change."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class VerilatorBuildTests(unittest.TestCase):
    """Build a tiny model through the production CMake integration."""

    def run_command(self, *command):
        """Run a build or model and retain diagnostics on failure."""
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_new_header_dependency(self):
        """Rebuild for a newly included header, then track later edits to it."""
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "gw/EconoPET"
            simulation = project / "sim"
            simulation.mkdir(parents=True)
            (project / "EconoPET.xml").write_text(
                '<efx:sim_file name="sim/probe_tb.sv"/>\n')
            (project / "verilator.vlt").write_text("`verilator_config\n")
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.21)
project(ModelDependencyTest LANGUAGES NONE)
add_custom_target(gateware_models)
include("{repository / 'cmake/VerilatorModels.cmake'}")
add_gateware_model(probe_tb)
""")
            bench = simulation / "probe_tb.sv"
            bench.write_text("""module probe_tb;
initial begin
    $display("VALUE=1");
    $finish;
end
endmodule
""")
            build = root / "build"
            self.run_command("cmake", "-S", str(root), "-B", str(build),
                             "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
            build_command = ("cmake", "--build", str(build),
                             "--target", "gateware_models", "--parallel", "2")
            executable = str(build / "gw-test/bin/probe_tb_vl")
            self.run_command(*build_command)
            self.assertIn("VALUE=1", self.run_command(executable))

            # Discover a dependency that did not exist during configuration.
            header = simulation / "value.svh"
            header.write_text("`define PROBE_VALUE 2\n")
            bench.write_text("""`include "sim/value.svh"
module probe_tb;
initial begin
    $display("VALUE=%0d", `PROBE_VALUE);
    $finish;
end
endmodule
""")
            self.run_command(*build_command)
            self.assertIn("VALUE=2", self.run_command(executable))

            # Editing only the new header must invalidate the executable.
            header.write_text("`define PROBE_VALUE 3\n")
            self.run_command(*build_command)
            self.assertIn("VALUE=3", self.run_command(executable))
            self.assertIn("no work to do", self.run_command(*build_command))


if __name__ == "__main__":
    unittest.main()
