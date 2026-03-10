#!/bin/bash

SCRIPT_DIR="$(readlink -f $(dirname "$0"))"
PROJ_NAME="EconoPET"
PROJ_DIR="$SCRIPT_DIR/gw/$PROJ_NAME"
BUILD_DIR="$SCRIPT_DIR/build"

generate_filelists() {
    mkdir -p "$PROJ_DIR/work_sim"

    python3 - "$PROJ_DIR/$PROJ_NAME.xml" "$PROJ_DIR/work_sim/$PROJ_NAME.f" "$PROJ_DIR/work_sim/pkgs.f" "$PROJ_DIR/work_sim/timescale.f" <<'PY'
import sys
import xml.etree.ElementTree as ET

xml_path, sim_f_path, pkgs_f_path, timescale_f_path = sys.argv[1:]

ns = {"efx": "http://www.efinixinc.com/enf_proj"}
root = ET.parse(xml_path).getroot()

design_files = [
    node.attrib["name"]
    for node in root.findall("./efx:design_info/efx:design_file", ns)
]
sim_files = [
    node.attrib["name"]
    for node in root.findall("./efx:sim_info/efx:sim_file", ns)
]

package_files = [path for path in design_files if path.endswith("_pkg.sv")]
design_files = [path for path in design_files if not path.endswith("_pkg.sv")]

# Header files are included from source and should not be compiled as top-level units.
sim_files = [path for path in sim_files if not path.endswith(".svh")]

with open(sim_f_path, "w", encoding="utf-8", newline="\n") as sim_f:
    for path in package_files + sim_files + design_files:
        sim_f.write(f"{path}\n")

# Keep this compatibility file for existing build trees that still reference it.
open(pkgs_f_path, "w", encoding="utf-8").close()

with open(timescale_f_path, "w", encoding="utf-8", newline="\n") as timescale_f:
    timescale_f.write("+timescale+1ns/1ps\n")
PY
}

# Helper function to check the exit code, pop the directory, and exit on failure
exit_on_failure() {
    local EXIT_CODE=$?
    if [ $EXIT_CODE -ne 0 ]; then
        popd
        exit $EXIT_CODE
    fi
}

show_help() {
    echo "Usage: $0 [OPTIONS] [TEST_NAME]"
    echo
    echo "Run gateware simulation tests."
    echo
    echo "Options:"
    echo "  -u, --update    Regenerate simulation file lists from project XML"
    echo "  -l, --lint      Run Verilator lint check"
    echo "  -m, --map       Run Efinity map flow after simulation"
    echo "  -v, --view      View waveform in GTKWave (requires TEST_NAME)"
    echo "  -a, --all       Run all tests via CTest"
    echo "  -h, --help      Show this help message"
    echo
    echo "Arguments:"
    echo "  TEST_NAME       Name of testbench to run (e.g., spi_tb, timing_tb)"
    echo "                  If omitted and -a not specified, lists available tests"
    echo
    echo "Examples:"
    echo "  $0 spi_tb       # Run spi_tb test"
    echo "  $0 -a           # Run all tests"
    echo "  $0 -v spi_tb    # View spi_tb waveform"
    echo "  $0              # List available tests"
}

TEST_NAME=""
while [[ $# -gt 0 ]]; do
  case $1 in
    -u|--update)
        UPDATE_F_FILE=1
        shift
        ;;
    -l|--lint)
        LINT=1
        shift
        ;;    
    -m|--map)
        EFX_MAP=1
        shift
        ;;
    -v|--view)
        VIEW_WAVE=1
        shift
        ;;
    -a|--all)
        RUN_ALL=1
        shift
        ;;
    -h|--help)
        show_help
        exit 0
        ;;
    -*)
        echo "Unknown option: $1"
        show_help
        exit 1
        ;;
    *)
        TEST_NAME="$1"
        shift
        ;;
  esac
done

if [ -n "$VIEW_WAVE" ]; then
    if [ -z "$TEST_NAME" ]; then
        echo "Error: --view requires a test name"
        echo "Usage: $0 -v TEST_NAME"
        exit 1
    fi
fi

if [ -n "$UPDATE_F_FILE" ]; then
    generate_filelists
    echo
fi

# Run all tests via CTest
if [ -n "$RUN_ALL" ]; then
    cd "$SCRIPT_DIR" && ctest --preset gw --output-on-failure
    exit $?
fi

# The generated file lists use project-relative paths. Therefore, execute iverilog
# from the root of the project directory.
pushd "$PROJ_DIR" || exit 1

if [ -n "$LINT" ]; then
    verilator --lint-only --language 1800-2009 --timescale-override 1ns/1ps -y src -Iexternal/m6502/rtl -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\" -f "$PROJ_DIR/work_sim/$PROJ_NAME.f" --top-module top
    exit_on_failure
fi

# If no test name provided, list available tests
if [ -z "$TEST_NAME" ]; then
    echo "Available testbenches:"
    for f in "$PROJ_DIR/sim/"*_tb.sv; do
        basename "$f" .sv
    done
    echo
    echo "Run a specific test:  $0 TEST_NAME"
    echo "Run all tests:        $0 -a"
    popd
    exit 0
fi

# Validate test name
if [ ! -f "$PROJ_DIR/sim/${TEST_NAME}.sv" ]; then
    echo "Error: Testbench '$TEST_NAME' not found"
    echo "Available testbenches:"
    for f in "$PROJ_DIR/sim/"*_tb.sv; do
        basename "$f" .sv
    done
    popd
    exit 1
fi

# Compile and run the specified test
VVP_FILE="$PROJ_DIR/work_sim/${TEST_NAME}.vvp"

iverilog -g2009 -s "$TEST_NAME" -o"$VVP_FILE" -f"$PROJ_DIR/work_sim/$PROJ_NAME.f" -f"$PROJ_DIR/work_sim/timescale.f" -Iexternal/m6502/rtl -DECONOPET_ROMS_DIR=\"${ECONOPET_ROMS_DIR}\"
exit_on_failure

vvp -l"$PROJ_DIR/outflow/${TEST_NAME}.rtl.simlog" "$VVP_FILE"
VVP_EXIT_CODE=$?

if [ -n "$VIEW_WAVE" ]; then
    gtkwave "$PROJ_DIR/work_sim/${TEST_NAME}.vcd" &
fi

if [ $VVP_EXIT_CODE -ne 0 ]; then
    popd
    exit $VVP_EXIT_CODE
fi

if [ -n "$EFX_MAP" ]; then
    "$SCRIPT_DIR/gw/efx.sh" --flow map
    exit_on_failure
fi

popd
