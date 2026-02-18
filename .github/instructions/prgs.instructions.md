---
description: 'C coding conventions for Commodore PET/CBM test and utility programs compiled with CC65'
applyTo: 'prgs/**/*.c, prgs/**/*.h, prgs/**/*.s, prgs/**/CMakeLists.txt'
---

# PET/CBM Program Guidelines

Example, utility, and test programs that run on the Commodore PET/CBM (WDC 65C02). Written in C, compiled with CC65 (`cl65 -t pet --cpu 65C02`), with inline or linked assembly where needed.

## File Header

```c
/* SPDX-License-Identifier: CC0-1.0 */
/* https://github.com/dlehenbauer/econopet */

/*
 * example.c -- One-line summary.
 *
 * Extended description of the test scenario or utility purpose.
 */
```

## Project Layout

Each program lives in its own subdirectory under `prgs/`. Name the subdirectory and main source file identically (e.g., `bp/bp.c`). Register new subdirectories in `prgs/CMakeLists.txt`. See `prgs/bp/CMakeLists.txt` for the build template.

## CC65-Specific C Conventions

- Prefer `unsigned char` over `int` for byte-sized values (CC65 generates far better code for 8-bit types)
- Declare counters and accumulators as `static` at file scope (CC65 generates more efficient code for statics than stack locals)
- Use `<conio.h>` (`cputs`, `cprintf`, `clrscr`) for text output, not `<stdio.h>` (`printf` pulls in a much larger runtime)
- Use `\r\n` for newlines in `cputs()`
- Use `PEEK()`/`POKE()` from `<peekpoke.h>` for direct memory access
- Use CC65/CA65 syntax (not ACME) for linked `.s` assembly files

## Test Program Pattern

1. Clear screen and print a banner identifying the test
2. Run each test case with a pass/fail check
3. Print a summary (`N/M passed`)