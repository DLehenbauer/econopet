## Scope
These rules apply ONLY to source files in `/workspaces/econopet/fw/external/PicoDVI/software/build/apps/colour_terminal`

## Brief overview
Project-specific guidelines for making changes to the colour_terminal app in the PicoDVI software directory.
Emphasizes incremental development with build verification at each step.

## Hardware Target
The colour_terminal application targets **ARM Cortex-M0+ on RP2040 hardware only**.

When comparing code from the PicoDVI repository, focus only on the ARM code paths that apply to RP2040.
Ignore or skip code enabled by these preprocessor defines:
- `PICO_RP2350` - RP2350-specific configurations
- `PICO_RISCV` - RISC-V specific implementations
- `__riscv` - RISC-V compiler intrinsics and assembly macros

## Build verification
- After any code change, verify the build using:
  ```
  cd /workspaces/econopet/fw/external/PicoDVI/software/build/apps/colour_terminal && cmake --build . --parallel 2>&1
  ```
- Do not proceed to the next change until the current change builds successfully
- Fix all build errors before moving forward

## Development approach
- Make small, incremental changes rather than large sweeping modifications
- Each change should be logically isolated and testable
- Prefer multiple small commits over one large change
- Test each modification individually before combining with other changes

## Consulting other samples
When working on `colour_terminal` and encountering design questions or implementation challenges:
- Examine other sample applications in `/workspaces/econopet/fw/external/PicoDVI/software/apps/` for similar patterns or solutions
- Review relevant drivers and libraries in `/workspaces/econopet/fw/external/PicoDVI/software/lib/` to understand core functionality
- Use these references as guides for idiomatic PicoDVI patterns, but adapt them appropriately for `colour_terminal`'s specific needs
- Document any patterns adopted from other samples in code comments for future reference

**Ignore code in `/fw/src`**, even though `/fw/src/video/*` may contain similar code to colour_terminal.

## Error handling workflow
- When a build error occurs, analyze the error message carefully
- Fix the immediate error before attempting additional changes
- If multiple errors exist, address them one at a time
- Re-run the build verification after each fix to confirm resolution
