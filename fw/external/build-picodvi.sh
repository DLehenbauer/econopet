# Build PicoDVI configured for EconoPET hardware.
# EconoPET may require extra time for 12 MHz crystal to stabilize.
# EconoPET uses same pinout as 'micromod_cfg'

pushd PicoDVI/software && rm -rf build && mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_COPY_TO_RAM=1 -DDVI_DEFAULT_SERIAL_CONFIG=micromod_cfg ..
make -j$(nproc)
pushd
