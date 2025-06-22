#!/bin/bash

pushd EconoPET && \
    source $EFINITY_HOME/bin/setup.sh && \
    $EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@" && \
    popd
