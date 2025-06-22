#!/bin/bash

source $EFINITY_PATH/bin/setup.sh
pushd EconoPET
$EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@"
cat outflow/EconoPET.warn.log
cat outflow/EconoPET.err.log
popd
