---
name: code-review
description: Review pull requests and code changes for correctness, security, test quality, and lifecycle regressions. Use for Copilot code review and PR review, including ownership, state transitions, failure retention, stale observations, alternate API paths, and incremental builds.
---

# Code Review

This skill is authoritative for the general review workflow. Scoped repository
instructions define the language and domain contracts to review against.

Perform the standard code review in addition to this workflow and scoped
checklists. Execution tracing and independent counterexamples supplement,
not replace, standard correctness, security, test, documentation, and convention
checks. Do not limit the review to the checks explicitly listed here.
Do not edit code unless fixes are requested.

## When to Use

Use when reviewing pull requests, branch diffs, or proposed changes to code,
build tooling, or documentation in any language or domain.

## Prerequisites and Context

Read the diff and intent. Discover scoped AGENTS.md files, instructions, and
skills for affected code and related callers/consumers. Take language/domain
requirements and evidence sources from that guidance.

## Workflow

### Establish the Changed Contracts

Identify affected contracts and compare with the base revision. Distinguish
introduced defects from unrelated pre-existing issues. Treat intent and tests
as evidence to examine, not proof. Follow interfaces beyond changed lines.

### Single-Step the Affected Paths

Simulate debugger single-stepping through each affected normal/error path,
including relevant unchanged callers and consumers. Track values, ownership,
pending work, and observable state against preconditions and postconditions.
Follow deferred effects, partial completion, cleanup, and recovery. Check that
decisions use current evidence and later operations see valid state.

Compare equivalent entry points and implementations for consistent guards and
effects. At boundaries, reconcile representation, ownership, ordering, and
failure semantics. Respect documented supported use.

### Derive and Explore Edge Cases

Derive cases from conditions, types, limits, and contracts, not historical
defects or existing tests. Vary inputs, ordering, repetition, failure timing,
and progress, including concurrency/reentry when applicable. Construct a
concrete trace for each suspected invariant violation.

For build changes, follow prerequisites, generated state, reuse, and
invalidation through the actual build/CI graph. For security findings,
establish trust boundaries and attacker control.

### Verify the Counterexamples

Establish the trigger, reachable contract violation, consequence, supporting
locations, and why guards do not prevent it. Check primary evidence and
supported-use documentation before promoting a hypothesis to a finding.

Run the smallest relevant existing validation available, using actual targets
and environment setup. Do not edit the worktree to reproduce a defect without
permission. Disclose missing evidence and validation.

Check that tests independently observe the contract rather than duplicate
implementation assumptions or hide differences. Identify untested paths and
states. A passing case establishes only what it measures.

On re-review, verify fixes and adjacent equivalent paths. Retain unresolved
findings and distinguish new defects from previously missed unchanged code.

### Report Supported Findings

Use Copilot's normal format/severity, prioritizing behavioral defects. Anchor
each finding to a useful changed location, explain the failing sequence, and
recommend a focused fix or regression. Include related locations as needed.
No quotas, duplicates, speculative warnings, or distracting style noise.
State validation results/limits without claiming absence of bugs.
