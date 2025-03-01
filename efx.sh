source /opt/efinity/2024.2/bin/setup.sh
pushd gw/EconoPET
$EFINITY_HOME/scripts/efx_run.py EconoPET.xml "$@"
popd
