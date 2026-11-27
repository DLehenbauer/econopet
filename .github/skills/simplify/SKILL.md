---
name: simplify
description: 'Review indicated code for simpler alternatives and cleaner architecture that preserve its goals and behavior. Use for /simplify, complexity reduction, architectural simplification, or asking what we would do differently if we reverted and reimplemented the code.'
user-invocable: true
---

# Simplify

Review the indicated code for simpler, cleaner alternatives that preserve its
goals and behavior. Ask for clarification if the target is unclear.

Run two independent subagents in parallel with the same target and user intent.
Both must follow applicable repository guidance.

- **Code reviewer:** Load and execute the
  [code-review skill](../code-review/SKILL.md). If activation is unavailable,
  read it in full and follow it. Stop if neither is possible.
- **Simplification reviewer:** Use this prompt:

  > Carefully review the code. Is there a simpler approach or an architecturally
  > cleaner alternative that achieves the same results? If you were going to
  > start over, what would you do differently?

Keep the reviews read-only. Neither subagent should launch more agents or
invoke this skill recursively. If subagents are unavailable, report the blocker.

Wait for both reviews, reconcile and deduplicate their findings, and report
evidence-backed recommendations with linked code locations. Separate correctness
defects from optional design improvements. State tradeoffs and validation limits.
If no worthwhile simplification emerges, say so.

Only implement changes if the user requested it, after combining both reviews.
