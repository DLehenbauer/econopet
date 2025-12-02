# EconoPET

Open hardware mainboard replacement for Commodore PET/CBM 2001/30xx/40xx/80xx machines. The EconoPET can also run standalone with HDMI display and USB keyboard.

See the [project page](https://dlehenbauer.github.io/econopet) for user manual and firmware updates.

## Workflow

```sh
# Configure
cmake --preset default

# Build
cmake --build --preset all          # Build everything
cmake --build --preset fw           # Build firmware only
cmake --build --preset fw_test      # Build firmware tests only
cmake --build --preset gw           # Build gateware only
cmake --build --preset rom          # Build ROMs only (menu and edit ROM)
cmake --build --preset sdcard       # Build SD card package

# Test
ctest --preset all                  # Run all tests
ctest --preset fw                   # Run firmware tests only
ctest --preset gw                   # Run gateware tests only
```
