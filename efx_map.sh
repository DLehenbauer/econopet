SCRIPT_DIR=$(realpath $(dirname $0))
pushd gw/EconoPET
rm -rf outflow
$SCRIPT_DIR/efx.sh -f map
cat outflow/EconoPET.warn.log
cat outflow/EconoPET.err.log
popd
