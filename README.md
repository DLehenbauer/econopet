# EconoPET

Open hardware mainboard replacement for Commodore PET/CBM 2001/30xx/40xx/80xx machines. The EconoPET can also run standalone with HDMI display and USB keyboard.

See the [project page](https://dlehenbauer.github.io/econopet) for user manual and firmware updates.

## Firmware Unit Tests

Firmware unit tests run on the Linux host using mock implementations of the EconoPET hardware.

```sh
cmake --build build --target test_project
ctest --test-dir build --output-on-failure --verbose -L fw
```
