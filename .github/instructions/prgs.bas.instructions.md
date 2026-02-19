---
description: 'BASIC coding conventions for Commodore PET/CBM test and utility programs compiled with petcat'
applyTo: 'prgs/**/*.bas'
---

# PET/CBM BASIC Program Guidelines

Test and utility programs written in Commodore BASIC for the PET/CBM. Source files use VICE `petcat` text format and are compiled to `.prg` with the `petcat` tool.

## Toolchain

Programs are compiled with `petcat` (from VICE). The standard invocation is:

```sh
petcat -h -v -l 401 -w1p -o output.prg -- input.bas
```

| Flag    | Purpose                                      |
| ------- | -------------------------------------------- |
| `-h`    | Write load address header in output           |
| `-v`    | Verbose output                                |
| `-l 401`| Set load address to $0401 (PET BASIC start)   |
| `-w1p`  | Target PET/CBM BASIC 1.0+ tokenization        |
| `-o`    | Output file path                              |

## Project Layout

Each program lives in its own subdirectory under `prgs/`. Name the subdirectory and the `.bas` source file identically (e.g., `hello/hello.bas`). Register new subdirectories in `prgs/CMakeLists.txt`.

### CMakeLists.txt Template

Use `prgs/hello/CMakeLists.txt` as the build template. Key elements:

```cmake
find_program(PETCAT_EXECUTABLE petcat REQUIRED)

set(PRG_NAME example.prg)

add_custom_command(
    OUTPUT ${BIN_DIR}/${PRG_NAME}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${PETCAT_EXECUTABLE} -h -v -l 401 -w1p -o ${BIN_DIR}/${PRG_NAME} -- ${SRC_DIR}/example.bas
    DEPENDS ${SRC_DIR}/example.bas
    COMMENT "Building ${PRG_NAME}"
    VERBATIM
)
```

## BASIC Coding Conventions

- Write all BASIC keywords in lowercase (`print`, `goto`, `peek`, `poke`)
- Use explicit line numbers, starting at 10 and incrementing by 10
- Use `peek()` and `poke` for direct memory access
- Keep lines short (under 80 characters) for readability
- Use `rem` for comments (sparingly, since they consume PET memory)

### Good Example

```basic
10 print "hello"
20 goto 10
```

### Bad Example

```basic
10 PRINT "HELLO"
20 GOTO 10
```

## Build and Verify

```sh
cmake --build --preset prgs    # Build all BASIC programs
```