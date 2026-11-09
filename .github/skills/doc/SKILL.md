---
name: doc
description: 'Document source code or refine existing comments and documentation. Use /doc to add IntelliSense documentation, explain intent and reasoning, or improve accuracy and clarity without fixing code.'
user-invocable: true
---

# Document source code

Document the indicated source or refine an indicated comment or documentation
passage for a senior engineer familiar with the language but not the code.
Explain intent, relevant contracts, and why non-obvious operations are correct.
Prefer clarity over brevity, but avoid unnecessary verbosity.

## Identify the scope

- Follow instructions supplied with `/doc` when they identify a region, file,
  or files.
- Otherwise, use the current IDE selection if one is available. If there is no
  selection, use the entire active file.
- If neither a target nor an active file is available, ask the user to identify
  the target. Read surrounding code as needed for context, but restrict edits to
  the target. A documentation comment may sit immediately outside a selected
  declaration when required by the language or IntelliSense.
- Treat attached editor snapshots as read-only evidence. Find the corresponding
  writable file and verify that it matches before editing. If it is missing or
  materially different, provide suggested documentation instead.

## Understand and document

- Read applicable repository and file-specific instructions. Trace relevant
  definitions, callers, signal drivers, consumers, invariants, ownership, and
  error handling before describing them. Do not infer correctness from names or
  existing comments. Distinguish current behavior from intent, requirements, and
  hardware guarantees. Follow applicable domain guidance for claims requiring
  ROM, hardware, or timing evidence.
- Give every type, member, global field, and function in scope documentation
  using the language's IntelliSense-compatible doc-comment convention. This
  includes classes, structs, enums, fields, methods, and enum values. Place
  documentation where the language tooling associates it with the declaration.
  Use the language's recognized documentation form, such as a Python docstring,
  when it has no doc-comment syntax.
- Explain purpose, contracts, and non-obvious rationale where useful. Document
  parameters, return values, side effects, ownership, constraints, and failure
  behavior when relevant. Avoid filler, tautologies, and unsupported claims.
- When refining existing documentation, correct factual errors, misleading
  omissions, ambiguous scope, and inconsistent terminology. Preserve useful
  rationale, conditions, units, exceptions, and limitations. Use precise domain
  terminology, active and direct language, and US English spelling, grammar,
  punctuation, and capitalization.
- In function and method bodies, precede each step of a multi-step procedure
  with an unnumbered comment describing what it does and any non-obvious
  rationale. Also explain other non-obvious logic. Separate a body comment from
  preceding code with one blank line. Do not insert a blank line between the
  opening brace and the first comment. Treat consecutive comment lines as one
  block.
- After writing each comment, ask whether there is a clearer way to describe
  it, then refine it. Use multiple sentences when they improve clarity.
- Follow local comment syntax, indentation, wrapping, and documentation format.
  Use plain ASCII prose and preserve identifiers, API names, established
  abbreviations, documentation tags, and intentional quotations.

## Apply and report

- Change only documentation, comments, and whitespace needed to format them.
  Do not fix or otherwise edit non-comment code. A language-recognized
  docstring may change documentation metadata, but must not change executable
  behavior. Preserve licenses, directives, annotations, and machine-significant
  comments.
- Report errors found in the code separately, with evidence and locations. Do
  not fix them or claim unverified intent or guarantees. If a fact cannot be
  established, state the uncertainty rather than inventing an explanation.
- Verify that the diff stays within scope and preserves executable behavior.
  Run existing documentation checks when applicable. Do not build or run code
  tests solely for ordinary documentation edits.
- Summarize substantive corrections. If no edit is needed, say so. If the
  target cannot be edited, show the suggested replacement and say that no file
  changed.
