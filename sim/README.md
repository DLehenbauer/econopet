# PET Emulator (`sim/`)

A host-side C++ software emulator of the Commodore PET. It is the starting point
for the software model of the "rest of the machine" in end-to-end simulation of the
EconoPET (Verilated gateware + firmware + this emulator). The end-to-end wiring is a
follow-up; today this is a standalone PET emulator.

## Origin

Vendored (first-party copy) from:

- Upstream: <https://github.com/dlehenbauer/pet-emulator>
- Branch: `main`
- Commit: `33cb2beb0397e172409069cea315f759d74830e9`

The `io` branch was intentionally NOT used: it drives real Raspberry Pi hardware
GPIO (`bcm_host`, `/dev/mem`) for hardware-in-the-loop operation, which does not
apply to a software simulation running in the devcontainer.

### Local modifications
- Removed the Raspberry Pi cross-compile helpers (`cmake/`, `setup-*.sh`) and the
  upstream `.vscode/`, `.devcontainer/`, and stale SDL2-boilerplate `README.md`.
- `CMakeLists.txt`: renamed the project (`EconoPET` -> `pet_emulator`), dropped the
  `file(COPY roms ...)` step, and modernized target properties.
- ROM loading now reads from the directory named by the `ECONOPET_ROMS_DIR`
  environment variable (falling back to `roms/`) instead of a bundled `roms/`
  directory. This reuses the repo's existing ROM solution
  (`.devcontainer/download-roms.sh`); the upstream `roms/get-roms.sh` was dropped.
- `display.cpp`: handle `SDL_QUIT` so closing the window exits the emulator.

## Dependencies
- SDL2 (`libsdl2-dev`), Boost (`libboost-dev`, `libboost-system-dev`), pthreads.
- The CPU core is `src/m6502.h` from `floooh/chips` (zlib license; see the repo
  `NOTICE.md`).

The devcontainer installs these automatically (see `.devcontainer/Dockerfile`).

## Build
From the repo root, using the super-project presets:

```sh
cmake --preset default      # configure (once)
cmake --build --preset sim  # builds build/sim/bin/pet_emulator
```

Or standalone:

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build     # builds sim/build/bin/pet_emulator
```

## Run
The emulator loads PET ROMs from `ECONOPET_ROMS_DIR` and opens an SDL2 window:

```sh
ECONOPET_ROMS_DIR=/path/to/roms ./build/sim/bin/pet_emulator
```

Close the window to exit. Function keys F1-F7 reset / load bundled demo programs
(see `main.cpp`).
