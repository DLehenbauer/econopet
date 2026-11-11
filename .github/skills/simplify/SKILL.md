---
name: simplify
description: 'Review indicated code for simpler alternatives and cleaner architecture that preserve its goals and behavior. Use for /simplify, complexity reduction, architectural simplification, or asking what we would do differently if we reverted and reimplemented the code.'
user-invocable: true
---

# Simplify

Answer the counterfactual: "If we reverted the indicated code and implemented
it again, what would we do differently?"

Review the design, not just its expression. Look for ways to remove unnecessary
concepts, state, dependencies, and coordination while preserving the required
behavior. Fewer lines alone do not establish a simpler design.

## When to Use

Use for a selected implementation, files, diff, commit, or pull request when
the user wants a careful simplification or architectural review. Do not treat
every correctness review as a request to redesign the code.

## Prerequisites and Boundaries

- Load the [code-review skill](../code-review/SKILL.md) and follow its workflow.
  If activation is unavailable, read it in full. Stop if neither is possible.
  Apply scoped repository, language, and domain guidance.
- Review by default. Do not edit, revert, reset, or discard code unless the
  user explicitly requests implementation. Reverting is a thought experiment.
- Work directly unless a separate, substantial investigation requires
  delegation. Do not split one design trace into overlapping reviews.

## Workflow

Track these phases in a session TODO list. Keep the investigation bounded to
the indicated implementation and the context needed to evaluate it.

### Establish Scope and Goals

Identify the exact selection, files, change range, or pull request. Read the
implementation and relevant base revision when reviewing changes. If no target
is indicated or the target is materially ambiguous, ask for clarification
rather than assuming every dirty file belongs to the task.

Read callers, consumers, tests, and directly related documentation. Summarize
the intended goals, observable behavior, invariants, and constraints. Separate
requirements from choices made by the current implementation. Include relevant
compatibility, resource, timing, concurrency, and failure-handling obligations.
State missing evidence instead of inventing requirements.

### Understand Why Complexity Exists

Trace representative normal, failure, cleanup, and retry paths. Identify who
owns data and pending work, where state changes, and which boundaries coordinate
those changes. Search for existing helpers, primitives, and equivalent paths
before proposing new abstractions.

Distinguish necessary domain complexity from accidental implementation
complexity. Check comments, history, and contracts before calling an awkward
path redundant. Do not remove safeguards merely because tests do not exercise
them.

### Reconsider the Design

Mentally set aside the implementation while retaining its goals and constraints.
Describe the simplest plausible design, then compare it with the current one.
Consider both local simplifications and a cleaner organizing approach:

- Remove redundant state, caches, flags, and synchronization by deriving values
  from an authoritative source when doing so preserves cost and ordering.
- Reduce representable invalid states and clarify ownership or lifecycle
  boundaries so fewer callers must coordinate correctly.
- Collapse unnecessary layers, pass-through wrappers, special cases, and
  one-off configuration. Reuse an existing mechanism where its contract fits.
- Replace duplicated policy with one source of truth without coupling unrelated
  behavior or hiding meaningful differences.
- Choose data representations and control flow that make invariants explicit.
  Prefer straightforward code over clever compression or generic frameworks.
- Reconsider misplaced responsibilities or dependency direction when a modest
  boundary change removes complexity across the affected paths.

Do not force a rewrite, a favored architectural pattern, or a quota of findings.
Retain the current approach when alternatives merely move complexity elsewhere.

### Challenge Each Alternative

For each credible candidate, identify the concrete code it replaces, the
mechanism it removes, and the total complexity remaining across callers,
consumers, tests, and build integration. Include migration cost and tradeoffs.

Trace the alternative through the same normal and edge cases as the current
design. Explicitly check behavior, error visibility, ownership, teardown,
ordering, and relevant resource or performance limits. Treat an intentional
behavior change as a separate proposal, not an equivalent simplification.

Use the smallest relevant existing validation when it can resolve a concrete
question without modifying the worktree. Separate demonstrated equivalence from
reasoned expectations and proposed regression tests. Do not claim an unbuilt
alternative has been validated. Reject candidates whose benefit depends on
unsupported assumptions or dropping requirements.

### Report the Recommendation

Start with a direct answer to the counterfactual. Prioritize a small number of
high-value, evidence-backed recommendations over cosmetic cleanup.

For each recommendation, provide:

- A linked code location and the specific complexity or coordination burden.
- The proposed replacement and why it is simpler overall.
- The behavior and constraints it must preserve, with supporting evidence.
- Tradeoffs, confidence, and any unresolved evidence or validation needs.
- A focused implementation path and the regression checks needed to adopt it.

Distinguish correctness defects from optional design improvements. Report
behavioral defects using the code-review workflow rather than disguising them
as simplifications. Summarize validation performed and its limits.

If no worthwhile alternative survives scrutiny, say so and briefly explain why
the existing design fits the requirements. Do not manufacture churn. If the
user also requested implementation, apply the selected changes, update directly
related documentation, and run the relevant existing checks before concluding.