---
description: 'Hardware-contract and lifecycle review checks for the EconoPET system-test simulator'
applyTo: 'test/system/**'
---

# System-Test Code Review Checklist

Apply this checklist within the mandatory
[code-review skill](../skills/code-review/SKILL.md) workflow, not as a standalone
review procedure.

Apply these checks to simulator adapters, fixtures, and execution frameworks.
Also follow the [C/C++ conventions](c-cpp.instructions.md) and the repository's
cross-component review guidance.

- [ ] Distinguish stimulus, evaluated model state, and cached observations.
  Verify adapters against production contracts, not simulator assumptions.
- [ ] Trace scheduling and callbacks across simulation phases. Check time
  accounting, ownership, mutation permissions, nesting, and progress.
- [ ] Verify fixture preconditions, rejection atomicity, failure retention,
  and observer teardown, including without simulated activity.
- [ ] Ensure fixtures, mocks, and observers independently measure the contract
  rather than duplicate or hide implementation assumptions.

Use selected-ROM evidence for native peripheral expectations. Do not treat a
fixture's encoding or a simulator's current output as the compatibility oracle.
