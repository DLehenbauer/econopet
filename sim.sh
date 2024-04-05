#!/bin/bash

PROJNAME="EconoPET"
PROJDIR="$(readlink -f $(dirname "$0"))/gw/$PROJNAME"

# Invoke 'efx_run' to generate/update the '\work_sim\<proj>.f' file, but ignore
# the resulting Python exception which occurs due to lack of SystemVerilog support.
# (See https://www.efinixinc.com/support/forum.php?cid=6&pid=932)
"$(dirname "$0")/efx.sh" --flow rtlsim 2> /dev/null
echo

# 'efx_run' produces relative paths to simulation files. Therefore, we must execute
# iverilog from the root of the project directory.
pushd "$PROJDIR" || exit 1

iverilog -g2009 -s "sim" -o"$PROJDIR/work_sim/$PROJNAME.vvp" -f"$PROJDIR/work_sim/$PROJNAME.f"
if [ $? -ne 0 ]; then
    popd && exit $?
fi

vvp -l"$PROJDIR/outflow/$PROJNAME.rtl.simlog" "$PROJDIR/work_sim/$PROJNAME.vvp"
popd && exit $?