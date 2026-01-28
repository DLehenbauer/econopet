---
description: 'C coding conventions and patterns for RP2040 firmware development'
applyTo: 'fw/**/*.c, fw/**/*.h'
---

# Firmware Development Guidelines

Instructions for developing C firmware for the RP2040 MCU using the Pico SDK.

## Header File Conventions

- `#pragma once` must be the first non-comment line
- Never include `pch.h` from header files

## Source File Include Order

Include files in this exact order with blank lines between groups:

1. `pch.h` (precompiled header)
2. Corresponding header for this source file
3. Blank line
4. Standard library headers `<...>` (alphabetized)
5. Blank line
6. External headers `"..."` (alphabetized)
7. Blank line
8. Project headers `"..."` (alphabetized)

### Good Example

```c
#include "pch.h"
#include "my_module.h"

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "driver.h"
#include "hw.h"
```

### Bad Example

```c
#include "hw.h"
#include <stdint.h>
#include "pch.h"  // Wrong: pch.h must be first
#include "my_module.h"
```

## Include Paths

- Use simple includes: `#include "pch.h"` or `#include "subdir/header.h"`
- Never use relative paths like `../../pch.h` (the include path already contains `fw/src`)

## Testing

- Tests use the [Check](https://libcheck.github.io/check/) framework
- See [fw/test/main.c](fw/test/main.c) for test structure examples

## Platform Abstraction

Use `PICO_PLATFORM` for conditional compilation between real hardware and host testing.

## Development Workflow

1. `cmake --build --preset fw` - build firmware
2. `cmake --build --preset fw_test` - build tests
3. `ctest --preset fw` - run tests

## External Dependencies

The `/fw/external` directory contains git submodules. Do not modify files in this path without explicit confirmation. When changes seem to require modifying submodule code:

1. Propose alternatives first (wrappers, adapters, compile flags)
2. Request confirmation before proceeding
