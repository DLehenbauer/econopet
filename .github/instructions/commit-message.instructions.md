---
description: 'Conventional Commits format for generated commit messages'
applyTo: ''
---

# Commit Message Format

Use the Conventional Commits standard (https://www.conventionalcommits.org/).

## Structure

```
<type>(<scope>): <description>

[optional body]
```

## Types

| Type | When to use |
|------|-------------|
| feat | A new feature or capability |
| fix | A bug fix |
| docs | Documentation only changes |
| style | Formatting, whitespace - no code change |
| refactor | Code change that neither fixes a bug nor adds a feature |
| test | Adding or updating tests |
| build | Changes to build system, CI, or dependencies |
| chore | Maintenance tasks that don't fit other types |

## Scopes

Use a short scope that identifies the component:

| Scope | When to use |
|-------|-------------|
| fw | Firmware changes (fw/) |
| gw | Gateware changes (gw/) |
| rom | ROM changes (rom/) |
| dev | VS Code settings, Dev container or Dockerfile changes (.devcontainer/) |

Omit scope when the change spans multiple components or does not fit a single scope.

## Rules

- Subject line: lowercase, imperative mood, no period at the end
- Keep the subject line under 72 characters
- Use plain ASCII (no smart quotes, em-dashes, or ellipsis characters)
- Add a body only when the "what" and "why" are not obvious from the subject
- Separate subject from body with a blank line
