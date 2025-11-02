## Brief overview
Project-specific guidelines for the EconoPET project emphasizing small, incremental changes with fast build/test feedback loops using CMake presets defined in CMakePresets.json.

## Incremental change philosophy
- Make one logical change at a time (single bug fix, isolated feature, or focused refactor)
- Validate each change using the fastest relevant build and test preset before moving to heavier builds
- Stop immediately on first failure; fix before proceeding with additional changes
- Avoid batching unrelated edits before validating earlier changes

## Firmware (/fw) development workflow
When making changes to firmware code:

1. Build tests first (fastest): `cmake --build --preset fw_test`
2. Run firmware tests: `ctest --preset fw`
3. Build full firmware image: `cmake --build --preset fw`

Rationale: `fw_test` builds much faster than the full `fw` target and catches some compile and unit test issues early. Only proceed to step 3 after steps 1-2 pass successfully.

## Gateware (/gw) development workflow
When making changes to gateware code:

1. Run Verilog simulation tests: `ctest --preset gw`
2. Only if tests pass, build gateware: `cmake --build --preset gw`

Rationale: The `gw` build target is very slow and not needed for simulation since tests use the Icarus Verilog interpreter. Never start the gateware build when simulation tests are failing.

## Cross-domain changes (fw + gw)
When changes span both firmware and gateware:
- Validate gateware simulation tests first (`ctest --preset gw`)
- Then run firmware test cycle (`cmake --build --preset fw_test` → `ctest --preset fw`)
- Only after both pass, proceed with full builds if needed

## Final verification
Once you believe a task is complete:

1. Build everything: `cmake --build --preset all`
2. Run all tests: `ctest --preset all`

Do not mark a task as complete until both commands succeed. This final verification ensures changes work correctly across the entire project.

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
1. `cmake --build --preset fw_test`
2. `ctest --preset fw`
3. `cmake --build --preset fw` (once tests pass)
4. `cmake --build --preset all` (final verification)
5. `ctest --preset all` (final verification)

Gateware-only change:
1. `ctest --preset gw`
2. `cmake --build --preset gw` (once simulation passes)
3. `cmake --build --preset all` (final verification)
4. `ctest --preset all` (final verification)
