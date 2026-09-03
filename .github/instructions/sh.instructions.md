---
description: 'Shell script conventions for EconoPET build, simulation, and tooling scripts'
applyTo: '**/*.sh'
---

# Shell Script Guidelines

Conventions for the `.sh` scripts that drive the build, simulation, and tooling flows (`build.sh`, `sim.sh`, `verilate.sh`, `seed.sh`, and the helpers under `.devcontainer/`, `gw/`, and `tools/`).

## Interpreter

- Start every shell script with `#!/usr/bin/env bash`. It resolves bash through `PATH`, so the same script works in the dev container and on a host where bash lives outside `/bin` (Homebrew on macOS, `/usr/local/bin` on BSD, Nix).
- Never omit the shebang. An executable script without one runs under `sh`, where bash builtins such as `pushd` and `[[` fail.
- Keep options out of the shebang. `env` does not pass flags portably, so put them in a `set` line instead.
- Leave a non-bash shebang alone. A few files carry a `.sh` extension but are not shell (`docs/dev/PET/scripts/*.sh` are perl and keep `#!/usr/bin/perl`).

## Executable Bit

Commit every `.sh` file mode `100755`. CMake and the scripts themselves invoke each other directly (`CTestCustom.cmake.in` runs `seed.sh --write` as a pre-test hook, `sim.sh` and `verilate.sh` exec `seed.sh`), so a non-executable script fails with "permission denied" on a fresh clone.

This repository sets `core.fileMode=false`, so `chmod +x` alone stages nothing and git never reports the drift. Set the bit in the index:

```sh
git update-index --chmod=+x path/to/script.sh
```

## File Header

Follow the shebang with the SPDX header (see `fw/src/main.c`), a one-line statement of purpose, and a `Usage:` line when the script takes arguments.

### Good Example

```sh
#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Compile and run a gateware testbench under Verilator (--binary --timing).
# Usage: ./verilate.sh TEST_NAME [RAND_RESET]
#   RAND_RESET: 0 (default; required for m6502), 1 (all ones), or 2 (random).

set -e
```

### Bad Example

```sh
#!/bin/bash -e
cd build
make
```

## Conventions

- Use `set -e` so a failed step stops the script. Add `-uo pipefail` when the script has pipelines or reads variables that may be unset.
- Quote expansions (`"$VAR"`, `"$@"`) and give optional arguments a default (`"${2:-0}"`).
- Resolve paths relative to the script, not the caller's working directory (`SCRIPT_DIR="$(readlink -f $(dirname "$0"))"`).
- Name scripts in lowercase with hyphens (`download-roms.sh`, `lcsc-import.sh`).

## Validation

```sh
bash -n path/to/script.sh                       # Syntax check (no execution)
git ls-files -s '*.sh' | awk '$1 != "100755"'   # Lists any script missing the executable bit
git grep -l '^#!/bin/bash' -- '*.sh'            # Lists any script with a hardcoded interpreter
```
