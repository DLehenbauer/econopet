# EconoPET

Open hardware mainboard replacement for Commodore PET/CBM 2001/30xx/40xx/80xx machines. The EconoPET can also run standalone with HDMI display and USB keyboard.

![EconoPET running Space Invaders](site/static/assets/EconoPET-Invaders.jpg)

## User Resources

See the [project page](https://dlehenbauer.github.io/econopet) for user manual and firmware updates.

## Manufacturing

> **⚠️ Warning:** Rev A is not perfect. Before ordering, review the currently [known issues](https://github.com/DLehenbauer/econopet/issues?q=state%3Aopen%20label%3A40%2F8096-A) and understand that new issues may be discovered over time.

Rev. A has been released. For manufacturing and assembly instructions, go [here](https://dlehenbauer.github.io/econopet/manual/build-an-econopet/).

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
ctest --preset all --parallel       # Run all tests
ctest --preset fw --parallel        # Run firmware tests only
ctest --preset gw --parallel        # Run gateware unit tests only
ctest --preset gw-boot --parallel   # Run full-boot benches (Verilator, minutes)
```

Gateware benches run under both simulators. `gw` runs both quick suites;
`gw-iv` and `gw-vl` run only Icarus or Verilator, respectively. Verilator is
two-state and settles differently, so it catches races Icarus tolerates. Each
suite takes under a minute. `ECONOPET_ROMS_DIR` must point at the ROM images:
the configure step requires it, and `top_tb` and the boot bench load real ROMs.

`gw-boot` is separate because it simulates seconds of PET time: it boots the
stock BASIC-4 ROMs on the soft 6502 and passes only once the banner, the RAM
sizing result and the `READY.` prompt are all on screen. That takes ~5 minutes
under Verilator and is impractical under Icarus. It prints the screen at the
end and writes `gw/EconoPET/outflow/stock6502_boot_tb.pgm`:

```
|*** COMMODORE BASIC 4.0 ***             |
|                                        |
| 31743 BYTES FREE                       |
|                                        |
|READY.                                  |
```

`ctest` hides that on success, so use `ctest --preset gw-boot -V` to see it, or
run `./verilate.sh stock6502_boot_tb 0` directly. See
[docs/dev/verilator.md](docs/dev/verilator.md) for the Verilator runner.

### Alpha builds

Every push to `main` that builds cleanly and passes the full test suite uploads
the resulting SD card package as a workflow artifact named
`EconoPET-40-8096-A-firmware-lkg-main.zip`. It is a plain build artifact (not a
published release) intended for alpha testers.

To download it, open the
[latest successful CI run on main](https://github.com/DLehenbauer/econopet/actions/workflows/ci.yml?query=branch%3Amain+is%3Asuccess)
and grab the artifact listed at the bottom of the run summary. Downloading
requires being signed in to GitHub. Unzip the contents onto the root of an SD
card. `BUILD-INFO.txt` inside records the commit, the build time and the CI run
that produced it, which is worth quoting in bug reports.

The `lkg-main` tag tracks the commit the artifact was built from. It only ever
moves forward along the history of `main`, so a build that finishes out of order
never regresses it: such a build withdraws its own package rather than leave a
stale download at the top of the run list. Packages from earlier commits stay on
their own run pages until they expire under the repository's retention policy.
Because `lkg-main` is force-updated, use `git fetch --tags --force` to follow it.

## License

This project is released under the [CC0 1.0 Universal](LICENSE) (CC0) license, placing it in the public domain.

**Exception:** external dependencies are subject to their own licenses as noted in their respective source code and in [NOTICE.md](NOTICE.md).

[![CI](https://github.com/DLehenbauer/econopet/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/DLehenbauer/econopet/actions/workflows/ci.yml)