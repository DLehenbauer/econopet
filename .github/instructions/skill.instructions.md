---
description: 'Guidelines for authoring AI agent skills following the Agent Skills specification'
applyTo: '**/SKILL.md'
---

# Agent Skills Authoring Guidelines

Instructions for creating skills that follow the Agent Skills specification (https://agentskills.io/specification).

## What Are Agent Skills?

Agent Skills are self-contained directories with instructions and bundled resources that teach AI agents specialized capabilities. Unlike custom instructions (which define coding standards), skills enable task-specific workflows that can include scripts, templates, examples, and reference data.

Key characteristics:

- Portable across VS Code, Copilot CLI, and Copilot coding agent
- Progressive loading (only loaded when relevant to the user's request)
- Resource-bundled (can include scripts, templates, examples alongside instructions)
- Activated automatically based on prompt relevance

## Directory Structure

Skills are stored in `.github/skills/<skill-name>/`. Each skill lives in its own directory named to match the `name` field in SKILL.md:

```
.github/skills/my-skill/
  SKILL.md              # Required: main instructions
  scripts/              # Optional: executable automation
  references/           # Optional: documentation loaded on demand
  assets/               # Optional: static files used as-is in output
  templates/            # Optional: starter code the agent modifies
```

## SKILL.md Format

### Frontmatter (Required)

Every SKILL.md must begin with YAML frontmatter containing at minimum `name` and `description`:

```yaml
---
name: skill-name
description: What this skill does and when to use it.
---
```

#### Required Fields

| Field | Limit | Rules |
|-------|-------|-------|
| name | 1-64 chars | Lowercase alphanumeric and hyphens only. No leading, trailing, or consecutive hyphens. Must match parent directory name. |
| description | 1-1024 chars | Describe what the skill does, when to use it, and include keywords for discovery. |

#### Optional Fields

| Field | Limit | Purpose |
|-------|-------|---------|
| license | -- | License name or reference to a bundled license file |
| compatibility | 1-500 chars | Environment requirements (product, system packages, network access) |
| metadata | key-value map | Arbitrary additional properties (use unique key names to avoid conflicts) |
| allowed-tools | space-delimited | Pre-approved tools the skill may use (experimental) |

### Writing Effective Descriptions

The `description` field is the primary mechanism for automatic skill discovery. Agents read only `name` and `description` to decide whether to load a skill. Include three things:

1. WHAT the skill does (capabilities)
2. WHEN to use it (specific triggers, scenarios, file types, or user requests)
3. KEYWORDS matching terms users would include in their prompts

Good:

```yaml
description: Toolkit for testing local web applications using Playwright. Use
  when asked to verify frontend functionality, debug UI behavior, capture browser
  screenshots, check for visual regressions, or view browser console logs.
  Supports Chrome, Firefox, and WebKit browsers.
```

Bad:

```yaml
description: Web testing helpers.
```

The bad example fails because it has no specific triggers, no keywords, and no capabilities.

### Body Content

The body contains detailed instructions loaded after the skill is activated. Recommended sections:

| Section | Purpose |
|---------|---------|
| Overview | Brief summary of what this skill enables |
| When to Use | Scenarios that trigger this skill (reinforces description) |
| Prerequisites | Required tools, dependencies, environment setup |
| Step-by-Step Workflows | Numbered steps for common tasks |
| Troubleshooting | Common issues and solutions |
| References | Links to bundled docs or external resources |

## Progressive Disclosure

Structure skills for efficient context usage. Agents load content in three tiers:

1. **Discovery (~100 tokens)**: `name` and `description` loaded at startup for all skills
2. **Instructions (target < 5000 tokens)**: Full SKILL.md body loaded when the request matches the description
3. **Resources (on demand)**: Files in `scripts/`, `references/`, `assets/`, and `templates/` loaded only when the agent references them

This means many skills can be installed without consuming context. Keep SKILL.md under 500 lines. Move detailed reference material to separate files.

## Bundling Resources

### Resource Types

| Directory | Purpose | Loaded into context? | Examples |
|-----------|---------|---------------------|----------|
| scripts/ | Executable automation | When executed | helper.py, validate.sh |
| references/ | Documentation the agent reads | When referenced | api_reference.md, schema.md |
| assets/ | Static files used as-is in output | No | logo.png, report-template.html |
| templates/ | Starter code the agent modifies | When referenced | scaffold.py, config.template |

### Assets vs Templates

- **Assets** are static resources consumed unchanged (a logo embedded into a generated document, a font applied to rendered text)
- **Templates** are starter code or scaffolds the agent actively modifies (a config file the agent fills in, a project directory the agent extends)

Rule of thumb: if the agent reads and builds upon the content, use `templates/`. If the file is used as-is in output, use `assets/`.

### Referencing Resources

Use relative paths from the skill root:

```markdown
See [API reference](./references/api_reference.md) for detailed documentation.

Run the extraction script:
scripts/extract.py
```

Keep references one level deep from SKILL.md. Avoid deeply nested reference chains.

## Writing Style

- Use imperative mood: "Run", "Create", "Configure" (not "You should run")
- Be specific and actionable
- Include exact commands with parameters
- Show expected outputs where helpful
- Keep sections focused and scannable

## Scripts

Scripts in `scripts/` must:

- Be self-contained or clearly document dependencies
- Include help/usage documentation (`--help` flag)
- Handle errors gracefully with clear messages
- Avoid storing credentials or secrets
- Use relative paths where possible

Prefer cross-platform languages:

| Language | Good for |
|----------|----------|
| Python | Complex automation, data processing |
| Node.js | JavaScript-based tooling |
| Bash/Shell | Simple automation tasks |

### When to Bundle Scripts

Include scripts when:

- The same code would be rewritten repeatedly by the agent
- Deterministic reliability is critical (file manipulation, API calls)
- Complex logic benefits from being pre-tested rather than generated each time
- The operation has a self-contained purpose that can evolve independently
- Predictable behavior is preferred over dynamic generation

### Security

- Rely on existing credential helpers (no credential storage in scripts)
- Include `--force` flags only for destructive operations
- Warn users before irreversible actions
- Document any network operations or external calls

### Scratch Data

- Store repo-local AI logs, traces, and temporary state under `.cache/ai/`

## Name Validation

Valid:
- `pdf-processing`
- `data-analysis`
- `code-review`

Invalid:
- `PDF-Processing` (uppercase not allowed)
- `-pdf` (cannot start with hyphen)
- `pdf-` (cannot end with hyphen)
- `pdf--processing` (consecutive hyphens not allowed)

## Workflow Execution Pattern

For multi-step workflows, create a TODO list where each step references the relevant documentation:

```markdown
## TODO
- [ ] Step 1: Configure environment (see [workflow-setup.md](./references/workflow-setup.md#environment))
- [ ] Step 2: Build project (see [workflow-setup.md](./references/workflow-setup.md#build))
- [ ] Step 3: Deploy to staging (see [workflow-deployment.md](./references/workflow-deployment.md#staging))
```

This ensures traceability and allows resuming workflows if interrupted. Split large workflows (more than 5 steps) into separate reference files.

## Validation Checklist

Before publishing a skill:

- [ ] SKILL.md has valid frontmatter with `name` and `description`
- [ ] `name` is lowercase with hyphens, at most 64 characters, matches directory name
- [ ] `description` clearly states WHAT it does, WHEN to use it, and relevant KEYWORDS
- [ ] Body includes when to use, prerequisites, and step-by-step workflows
- [ ] If SKILL.md exceeds 500 lines, warn the user but do not shorten it unless asked
- [ ] Large workflows split into `references/` with clear links from SKILL.md
- [ ] Scripts include help documentation and error handling
- [ ] Relative paths used for all resource references
- [ ] No hardcoded credentials or secrets

## Additional Resources

- [Agent Skills Specification](https://agentskills.io/specification)
- [Awesome Copilot Agent Skills Instructions](https://github.com/github/awesome-copilot/blob/main/instructions/agent-skills.instructions.md)
