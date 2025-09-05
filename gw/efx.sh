#!/bin/bash

source $EFINITY_HOME/bin/setup.sh

pushd $(dirname "$0")/EconoPET && \
    python3 -u $EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@" && \
    popd
