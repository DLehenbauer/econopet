## Scope
These rules apply ONLY to source files in `/workspaces/econopet/fw/external/PicoDVI/software/build/apps/colour_terminal`

## Brief overview
Project-specific guidelines for making changes to the colour_terminal app in the PicoDVI software directory.
Emphasizes incremental development with build verification at each step.

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

## Error handling workflow
- When a build error occurs, analyze the error message carefully
- Fix the immediate error before attempting additional changes
- If multiple errors exist, address them one at a time
- Re-run the build verification after each fix to confirm resolution
