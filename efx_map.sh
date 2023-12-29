source /opt/efinity/2023.2/bin/setup.sh
pushd gw/EconoPET
rm -rf outflow
/opt/efinity/2023.2/scripts/efx_run.py EconoPET.xml -f map
cat outflow/EconoPET.warn.log
cat outflow/EconoPET.err.log
popd
