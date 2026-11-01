---
name: comment
description: 'Improve documentation and source comments. Use /comment to refine a selected comment for factual accuracy, correctness, consistent domain terminology, US English spelling and grammar, and conciseness without sacrificing readability or clarity.'
user-invocable: true
---

# Comment

Improve documentation without changing executable behavior. Prefer a clear,
accurate explanation over a shorter but ambiguous one.

## When to Use

- Refine a selected source comment or documentation passage.
- Correct misleading descriptions, inconsistent terminology, spelling, or grammar.
- Improve documentation for an explicitly identified symbol or region.

## Prerequisites

- Read applicable repository and file-specific instructions before editing.
- Identify the selected text and its surrounding implementation.
- Treat attached editor snapshots as read-only evidence, not editable files.

## Workflow

### Identify the scope

Use the selected comment as the primary target. If the selection contains code,
edit only its comments unless the user explicitly requests code changes. Without
a selection, use an explicitly named documentation passage, symbol, or region.
If no target is identifiable, ask for one instead of rewriting unrelated text.

### Verify the meaning

Read the surrounding implementation and follow relevant definitions, callers,
signal drivers, and consumers as needed to verify each factual claim. Do not
treat existing comments or suggestive names as proof.

Distinguish current behavior from intent, requirements, and hardware guarantees.
Use applicable domain guidance for claims that require ROM, hardware, or timing
evidence. If a claim cannot be verified, explain the uncertainty rather than
inventing an authoritative replacement.

### Refine the wording

- Correct factual errors, misleading omissions, and ambiguous scope.
- Use precise domain terminology consistently with verified repository usage.
  Preserve distinctions such as clock versus clock enable, bus reservation versus
  ownership, and address decoding versus register mirroring.
- Correct US English spelling, grammar, punctuation, and capitalization.
- Use active, direct language and remove repetition and filler.
- Preserve useful rationale, conditions, units, exceptions, and limitations.
- Prefer readability and clarity over the fewest words. Do not merely restate
  obvious code.
- Preserve identifiers, API names, and established abbreviations verbatim.
- Follow local comment syntax, indentation, wrapping, and documentation format.
  Use plain ASCII prose.
- Preserve license notices, tool directives, documentation tags, and intentional
  quotations. Do not change machine-significant text as a stylistic edit.

### Apply and validate

Make surgical documentation-only edits in the writable workspace. When working
from a read-only snapshot, locate and inspect the corresponding workspace text
before editing. If it is missing or materially different, provide a suggested
replacement instead of overwriting divergent content.

Check that edits preserve executable behavior and documentation structure.
Run existing documentation checks when applicable. Do not build or run code
tests solely for ordinary prose edits.

Briefly summarize substantive corrections. If no edit is needed, say so. When
the target cannot be edited, show the replacement and state that no file changed.
