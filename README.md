# EconoPET

Open hardware mainboard replacement for Commodore PET/CBM 2001/30xx/40xx/80xx machines. The EconoPET can also run standalone with HDMI display and USB keyboard.

![EconoPET running Space Invaders](site/dist/assets/EconoPET-Invaders.jpg)

## User Resources

See the [project page](https://dlehenbauer.github.io/econopet) for user manual and firmware updates.

## Manufacturing

Rev. A has been released. For manufacturing and assembly instructions, see [docs/manufacturing.md](docs/manufacturing.md).

## Development

This project uses a [Dev Container](https://containers.dev/) for a consistent development environment on Windows, macOS, and Linux.

### Prerequisites

- [Docker](https://www.docker.com/)
- [VS Code](https://code.visualstudio.com/)
- [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)

### Workflow

This project uses [CMake](https://cmake.org/) to manage the build process for firmware, gateware, ROMs, and SD card package.

```sh
# Configure
cmake --preset default              # Do this first

# Build
cmake --build --preset all          # Build everything
cmake --build --preset fw           # Build firmware only
cmake --build --preset fw_test      # Build firmware tests only
cmake --build --preset gw           # Build gateware only
cmake --build --preset rom          # Build ROMs only
cmake --build --preset sdcard       # Build SD card package

# Test
ctest --preset all                  # Run all tests
ctest --preset fw                   # Run firmware tests only
ctest --preset gw                   # Run gateware tests only
```

## License

This project is released under the [CC0 1.0 Universal](LICENSE) (CC0) license, placing it in the public domain.

**Exception:** external dependencies are subject to their own licenses as noted in their respective source code and in [NOTICE.md](NOTICE.md).
