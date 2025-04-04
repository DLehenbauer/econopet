#!/bin/bash

SCRIPT_DIR="$(readlink -f $(dirname "$0"))"
PROJ_NAME="EconoPET"
PROJ_DIR="$SCRIPT_DIR/gw/$PROJ_NAME"

# Helper function to check the exit code, pop the directory, and exit on failure
exit_on_failure() {
    local EXIT_CODE=$?
    if [ $EXIT_CODE -ne 0 ]; then
        popd
        exit $EXIT_CODE
    fi
}

while [[ $# -gt 0 ]]; do
  case $1 in
    -u|--update)
        UPDATE_F_FILE=1
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
    *)
        echo "Unknown option: $1"
        exit 1
        ;;
  esac
done

if [ -n "$VIEW_WAVE" ]; then
    gtkwave "$PROJ_DIR/work_sim/out.vcd" &
    exit $?
fi

if [ -n "$UPDATE_F_FILE" ]; then
    # Invoke 'efx_run' to generate/update the '\work_sim\<proj>.f' file, but ignore
    # the resulting Python exception which occurs due to lack of SystemVerilog support.
    # (See https://www.efinixinc.com/support/forum.php?cid=6&pid=932)
    "$(dirname "$0")/efx.sh" --flow rtlsim 2> /dev/null
    echo
fi

# 'efx_run' produces relative paths to simulation files. Therefore, we must execute
# iverilog from the root of the project directory.
pushd "$PROJ_DIR" || exit 1

iverilog -g2009 -s "sim" -o"$PROJ_DIR/work_sim/$PROJ_NAME.vvp" -f"$PROJ_DIR/work_sim/$PROJ_NAME.f" -f"$PROJ_DIR/work_sim/timescale.f" -Iexternal/65xx
exit_on_failure

vvp -l"$PROJ_DIR/outflow/$PROJ_NAME.rtl.simlog" "$PROJ_DIR/work_sim/$PROJ_NAME.vvp"
exit_on_failure

if [ -n "$EFX_MAP" ]; then
    "$SCRIPT_DIR/efx_map.sh"
    exit_on_failure
fi

popd
