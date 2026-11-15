---
name: sync-mockup
description: Apply requested Petmate menu mockup changes to EconoPET firmware. Use for /sync-mockup or syncing fw/src/ui/mockups.petmate layout, labels, ordering, and navigation to menu code and tests.
---

# Sync Menu Mockup

Apply only user-requested differences. Mockups establish text and geometry, not
callback behavior. Ask about ambiguous behavior. Do not edit the mockup source
unless explicitly requested.

## Key Files

| File | Purpose |
|---|---|
| `fw/src/ui/mockups.petmate` | Minified JSON, 40x25 screens in `.framebufs[]`, with `.name` and `.framebuf[row][col].code` |
| `fw/src/menu/menu_ui.c`, `menu_ui.h` | Controls, callbacks, state, geometry, and rendering |
| `fw/src/ui/tui.c` | Shared rendering and navigation |
| `fw/src/menu/menu_config.c`, `menu.c` | Configuration loading, Reset, and Resume |
| `fw/test/menu_ui_test.c` | Rendered text, geometry, reverse-video selection, navigation, and outcomes |
| `fw/test/menu_session_test.c` | Runtime menu-owner behavior across Resume, Reset, and Change Config |

## Workflow

- [ ] Inspect only the requested framebuffer(s) in `fw/src/ui/mockups.petmate`.
  Select by `.name` (list names first if needed), rather than dumping all screens.
- [ ] Trace affected controls in `fw/src/menu/menu_ui.c` through their
  `tui_screen_fn` definitions. Preserve label/action pairing and reorder label
  and callback arrays together. Follow `menu_config.c` and `menu.c` for changes
  affecting Reset, Resume, or configuration loading.
- [ ] Update affected expectations in `fw/test/menu_ui_test.c` and
  `fw/test/menu_session_test.c`. Search `fw/src/menu` and `fw/test` for changed
  literals and fixed row/component indices. Check rendered text, reverse-video
  selection, navigation targets, and runtime-owner behavior when Reset,
  Change Config, or Back moves.
- [ ] Validate with the commands below. Menu-only Check selections suffice
  during iteration, but finish with the full firmware preset to cover both UI
  and runtime-owner contracts.

```sh
cmake --build --preset fw-test
ctest --preset fw --parallel --output-on-failure
git diff --check
```
