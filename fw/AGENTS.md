# Firmware Development Guidelines

Instructions for developing C firmware for the RP2040 MCU using the Pico SDK.
Also follow the [general C/C++ guidelines](../.github/instructions/c-cpp.instructions.md).

## Precompiled Header and Include Paths

- Include `pch.h` first in firmware source files, before the corresponding header
- The firmware include path already contains `fw/src`

## Testing

- Firmware unit tests use the [Check](https://libcheck.github.io/check/) framework
- See [fw/test/main.c](test/main.c) for test structure examples

## Platform Abstraction

Use `PICO_PLATFORM` for conditional compilation between real hardware and host testing.

## Development Workflow

1. `cmake --build --preset fw` - build firmware
2. `cmake --build --preset fw-test` - build tests
3. `ctest --preset fw --parallel` - run tests

## External Dependencies

The `/fw/external` directory contains git submodules. Do not modify files in this path without explicit confirmation. When changes seem to require modifying submodule code:

1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding
