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

- Prefer [test_support](../test/support/) fakes/mocks over host-test `#if` branches to exercise production code paths
- Select hardware/test implementations with CMake `PICO_PLATFORM`. Use C `PICO_RP2040` conditionals only when test-support implementations cannot reasonably replace them
- Use `count_of(array)` instead of `sizeof(array) / sizeof(array[0])`

## Code Review Checklist

Apply this checklist within the mandatory
[code-review skill](../.github/skills/code-review/SKILL.md) workflow, not as a
standalone review procedure.

- [ ] Trace hardware/software state through startup, asynchronous events,
  deferred work, and recovery. Check ordering, observation freshness, and
  ownership across main-loop and interrupt boundaries.
- [ ] Check capacity and encoding contracts at shared-helper boundaries.
- [ ] Verify diagnostics and recovery are usable at the failure's lifecycle stage.

## Development Workflow

1. `cmake --build --preset fw` - build firmware
2. `cmake --build --preset fw-test` - build tests
3. `ctest --preset fw --parallel` - run tests

## External Dependencies

The `/fw/external` directory contains git submodules. Do not modify files in this path without explicit confirmation. When changes seem to require modifying submodule code:

1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding
