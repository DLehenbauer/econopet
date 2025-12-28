## Brief overview
Coding conventions for C code in the EconoPET project (`fw/` directory).

## Header file rules
- Headers must never include `pch.h`
- Headers must have `#pragma once` as the first non-comment line

## Source file include order
1. `pch.h` must be the first include
2. The corresponding header file for the source file must be second (e.g., `driver.c` includes `driver.h` second)
3. Blank line separator
4. Standard library includes using angle brackets `#include <...>`, alphabetized
5. Blank line separator
6. External includes using quotes `#include "..."`, alphabetized
7. Blank line separator
8. Project includes using quotes `#include "..."`, alphabetized

## Include path conventions
- The include path for `fw/` includes `fw/src`, so relative paths like `../../pch.h` should never be used
- Always use simple includes like `#include "pch.h"` or `#include "subdir/header.h"`

## Example source file
```c
#include "pch.h"
#include "sd.h"

#include <stdint.h>
#include <string.h>

#include "blockdevice/flash.h"
#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/littlefs.h"
#include "filesystem/vfs.h"

#include "display/dvi/dvi.h"
#include "fatal.h"
#include "global.h"
```
