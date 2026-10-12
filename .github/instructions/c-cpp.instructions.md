---
description: 'General C/C++ coding conventions for project code, including system integration tests'
applyTo: '**/*.c, **/*.cpp, **/*.cc, **/*.cxx, **/*.h, **/*.hpp, **/*.hh, **/*.hxx'
---

# C/C++ Development Guidelines

Apply these conventions to project-owned C/C++ sources and headers, including
firmware, shared test support, and system integration tests. Follow additional
target-specific instructions for toolchain and platform requirements. Do not
rewrite external dependencies to enforce these conventions.

## Build

Choose compilation settings by each target's role, not by the final executable
it links into. The Verilated code row takes precedence for models and runtime
linked into tests.

| Code being compiled | Configuration | Required settings |
| --- | --- | --- |
| Production code for deployment | `RelWithDebInfo` | Keep debug symbols and define `NDEBUG`. |
| Test code, simulator integration, test frameworks, and their dependencies (including production sources compiled for tests) | `Debug` | Keep `NDEBUG` undefined so C/C++ assertions and debug-only checks remain active. |
| Verilator-generated models and Verilator runtime | Fully optimized `RelWithDebInfo` | Use Verilator `-O3`, C++ `-O3`, native CPU tuning, and LTO. Preserve debug symbols and SV assertions. |

- Compile Verilated models and runtime in separate targets. Do not propagate
  their optimization flags or `NDEBUG` to linked test, framework, or firmware
  targets.
- Do not add Release builds or cross-configuration matrices, or undefine `NDEBUG`.

## Invariants and Test Expectations

- Prefer compile-time `static_assert` and `_Static_assert` checks when possible.
- Enforce production invariants with `vet()` from `fatal.h`.
- Use `assert()` and `#ifndef NDEBUG` as testing conveniences to check internal
  invariants that are difficult or impossible to observe externally (active in
  Debug tests, elided from production).
- Use GoogleTest `EXPECT_*`/`ASSERT_*` or Check `ck_assert_*` for expectations
  and helper checks within the respective framework.
- Keep required validation, work, and side effects outside assertions and
  `NDEBUG` guards.
- Fail stubs explicitly. Never return placeholders or succeed.

## Code Review Checklist

Apply this checklist within the mandatory
[code-review skill](../skills/code-review/SKILL.md) workflow, not as a standalone
review procedure.

- [ ] Trace ownership, lifetime, aliasing, and invalidation across object
  operations and callbacks. Verify borrowed storage and class invariants remain
  valid on normal, exceptional, and concurrent paths.
- [ ] Evaluate expressions under actual C/C++ type and conversion rules.
  Check range and undefined behavior before assuming mathematical semantics.
- [ ] Rely on language/library guarantees, not implementation accidents.
  Verify exception guarantees and cleanup after partial mutation.

## Header File Conventions

- `#pragma once` must be the first non-comment line
- Never include `pch.h` from header files

## Source File Include Order

Include files in this order with blank lines between groups:

1. `pch.h` (when the target uses a precompiled header), followed by the corresponding header for this source file (when one exists)
2. Standard library headers `<...>` (alphabetized)
3. External headers (alphabetized, using the dependency's include style)
4. Project headers `"..."` (alphabetized)

Do not add a precompiled header to targets that do not use one.

### Good Example (With a Precompiled Header)

```c
#include "pch.h"
#include "my_module.h"

#include <stdbool.h>
#include <stdint.h>

#include "external/library.h"

#include "project/helper.h"
```

### Bad Example

```c
#include "project/helper.h"
#include <stdint.h>
#include "pch.h"  // Wrong: pch.h must be first when used
#include "my_module.h"
```

## Include Paths

- Use include-directory-relative paths: `#include "my_module.h"` or `#include "subdir/header.h"`
- Never use parent-relative paths like `../../pch.h`
- Configure the target's include directories instead of traversing the source tree in includes

## Const Correctness

- Default all variables, parameters, and pointers to `const` or `constexpr` when possible
- Use `const` pointee types (`const T *`) unless the pointed-to data is modified

## Enums and Constants

- Use enums for natural categories such as states, selections, and opcodes.
  Use `constexpr`, `const`, or `#define` for other constants. Do not group
  unrelated constants in an enum.
