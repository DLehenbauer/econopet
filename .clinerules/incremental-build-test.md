## Brief overview
Project-specific guidelines for the EconoPET project emphasizing small, incremental changes with fast build/test feedback loops using CMake presets defined in CMakePresets.json.

## Incremental change philosophy
- Make one logical change at a time (single bug fix, isolated feature, or focused refactor)
- Validate each change using the fastest relevant build and test preset before moving to heavier builds
- Stop immediately on first failure; fix before proceeding with additional changes
- Avoid batching unrelated edits before validating earlier changes

## Gateware (/gw) development workflow
When making changes to gateware code:

1. Run Verilog simulation tests: `ctest --preset gw`
2. Only if tests pass, build gateware: `cmake --build --preset gw`

Rationale: The `gw` build target is very slow and not needed for simulation since tests use the Icarus Verilog interpreter. Never start the gateware build when simulation tests are failing.

## Command reference
Consult `CMakePresets.json` and `README.md` for complete information about available CMake build and test presets.

## External submodules
Directories `/fw/external` and `/gw/EconoPET/external` contain third-party git submodules. Never modify files in these paths or their subdirectories without explicit user confirmation.

General tasks like "fix compiler warnings" or "refactor for testing" should be assumed to exclude git submodules unless the user explicitly indicates otherwise. When unsure whether a task should include submodule files, ask the user for clarification.

When a solution requires changing submodule code:
1. Propose alternatives first: wrapper functions, adapter layers, compile flags, etc.
2. If no alternative exists: describe the required changes and request confirmation before proceeding
3. Verify submodule boundaries via `git submodule status` or `.gitmodules`

Rationale: Avoids maintaining forks and prevents upstream divergence.

## Typical workflow examples
Firmware-only change:
1. `cmake --build --preset fw`
2. `cmake --build --preset fw_test`
3. `ctest --preset fw`

Gateware-only change:
1. `ctest --preset gw`
2. `cmake --build --preset gw` (once simulation passes)
