source /opt/efinity/2024.1/bin/setup.sh
pushd gw/EconoPET
$EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@"
popd
