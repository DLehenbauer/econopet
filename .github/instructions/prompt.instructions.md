---
description: 'Guidelines for authoring effective prompt files for an AI coding agent'
applyTo: '**/*.prompt.md'
---

# Prompt File Authoring Guidelines

Instructions for creating reusable prompt files (slash commands) that encode common AI coding tasks as standalone Markdown files. Prompt files live in `.github/prompts/` and are invoked manually in chat via `/prompt-name`.

## When to Use Prompt Files

Prompt files are best for repeatable, task-specific workflows that a developer triggers on demand. Use them to:

- Scaffold common components or modules
- Run and fix tests with project-specific conventions
- Prepare pull request descriptions
- Perform security or code reviews
- Generate boilerplate following project patterns

Do not use prompt files for passive guidance that should always apply. Use custom instructions (`.instructions.md` files) for that instead.

## File Location and Naming

| Scope | Location |
|-------|----------|
| Workspace | `.github/prompts/` directory |
| User profile | `prompts/` folder of the current VS Code profile |

- Name files with lowercase and hyphens: `create-component.prompt.md`, `fix-tests.prompt.md`
- The filename (without `.prompt.md`) becomes the default slash command name

## File Format

### Frontmatter

Begin each prompt file with YAML frontmatter to configure its behavior. All fields are optional:

```yaml
---
description: Short description shown when browsing prompts
name: slash-command-name
agent: agent
model: copilot-chat-model-name
tools:
  - tool-or-toolset-name
  - mcp-server-name/*
---
```

| Field | Required | Purpose |
|-------|----------|---------|
| description | No | Short description of what the prompt does |
| name | No | Name used after typing `/` in chat. Defaults to filename. |
| argument-hint | No | Hint text shown in the chat input to guide user interaction |
| agent | No | Agent to run the prompt: `ask`, `agent`, `plan`, or a custom agent name. Defaults to `agent` when tools are specified. |
| model | No | Language model to use. Defaults to the currently selected model. |
| tools | No | List of tool or tool set names available to the prompt. Use `server-name/*` for all tools from an MCP server. Unavailable tools are silently ignored. |

### Body

Write the prompt text in the body using Markdown format. Include the specific instructions, context, and guidelines for the AI to follow.

### Body Structure

Start with an `#` heading that matches the prompt's intent so it surfaces well in Quick Pick search. Organize the body into predictable sections that follow a logical flow:

1. **Mission**: What the prompt accomplishes (one or two sentences)
2. **Context**: Background information, file references, and preconditions
3. **Inputs**: Variables, user-supplied values, and how to handle missing input
4. **Workflow**: Step-by-step instructions for the AI to follow
5. **Output Expectations**: Format, structure, and location of results
6. **Validation**: How to verify the output is correct

Adjust section names to fit the domain, but retain the overall flow: why, context, inputs, actions, outputs, validation.

## Writing Effective Prompts

### Be Clear and Specific

State exactly what the prompt accomplishes and what output format to expect. Vague prompts produce inconsistent results.

Good:

```markdown
Generate a SystemVerilog testbench for the module defined in the attached file.
Include clock generation, reset sequence, and at least three test cases that
exercise the primary input combinations.
```

Bad:

```markdown
Write some tests.
```

### Provide Examples

Show the AI what good input and output look like. Concrete examples anchor behavior far more effectively than abstract descriptions.

```markdown
## Expected Output

A C function following this pattern:

\`\`\`c
static bool cmd_example(const char *args) {
    // Parse arguments
    // Execute command
    // Return true on success
}
\`\`\`
```

### Reference Files with Markdown Links

Use relative Markdown links to pull in workspace files as context. Paths are resolved relative to the prompt file location.

```markdown
Follow the coding conventions in [firmware instructions](../instructions/firmware.instructions.md).

Use [driver.h](../../fw/src/driver.h) as the interface reference.
```

This avoids duplicating guidelines across prompts and keeps a single source of truth.

### Reference Tools Inline

Use the `#tool:<tool-name>` syntax in the prompt body to reference agent tools:

```markdown
Use #tool:githubRepo to look up open issues before generating the fix.
```

### Use Variables for Flexibility

Built-in variables make prompts reusable across different files and contexts:

| Variable | Expands to |
|----------|------------|
| `${file}` | Full path of the active file |
| `${fileBasename}` | Filename of the active file |
| `${fileBasenameNoExtension}` | Filename without extension |
| `${fileDirname}` | Directory of the active file |
| `${selection}` / `${selectedText}` | Currently selected text in the editor |
| `${workspaceFolder}` | Root path of the workspace |
| `${workspaceFolderBasename}` | Name of the workspace folder |
| `${input:varName}` | Prompts the user for free-text input |
| `${input:varName:placeholder}` | Same, with placeholder hint text |

Example using variables:

```markdown
---
description: Review the current file for common issues
tools:
  - codebase
---

Review `${file}` for:
- Violations of project coding conventions
- Potential bugs or off-by-one errors
- Missing error handling
```

### Handle Missing Context

Document how the AI should proceed when mandatory input is absent. Provide defaults or fallback behavior so the prompt does not silently produce wrong output.

```markdown
The target module is: ${input:moduleName:e.g. spi}

If no module name is provided, ask the user for it before proceeding.
```

### Keep Prompts Focused

Limit each prompt file to one task. A prompt that tries to scaffold, test, document, and deploy produces mediocre results across all four. Split compound workflows into separate prompts or use a prompt that coordinates sub-steps.

### Link to Instructions Instead of Duplicating

Reference shared `.instructions.md` files rather than repeating project conventions in every prompt:

```markdown
Follow the conventions in [C firmware guidelines](../instructions/firmware.instructions.md)
and [gateware guidelines](../instructions/gateware.instructions.md).
```

### Tone and Style

- Write in direct, imperative sentences addressed to the AI: "Analyze", "Generate", "Create" (not "You should analyze")
- Keep sentences short and unambiguous
- Avoid idioms, humor, or culturally specific references
- Use neutral, inclusive language

### Define Expected Output

- Specify the format, structure, and file location of results (for example, "Create a test file at `fw/test/test_<name>.c`")
- Include success criteria so the AI knows when the task is complete
- Include failure triggers so the AI knows when to stop and ask for clarification
- Provide validation steps (commands to run, checks to perform) that confirm correctness

## Tool List Priority

When both a prompt file and a referenced custom agent specify tools, the effective tool list follows this priority:

1. Tools specified in the prompt file (highest priority)
2. Tools from the referenced custom agent
3. Default tools for the selected agent (lowest priority)

## Tool and Permission Guidance

- Limit `tools` to the smallest set that enables the task (least-privilege)
- List tools in preferred execution order when sequence matters
- Warn about destructive operations (file overwrites, terminal commands with side effects) and include guard rails or confirmation steps in the workflow
- If the prompt inherits tools from a custom agent, state that relationship and note any critical tool behaviors or side effects

## Example Prompt File

```markdown
---
description: Create a new SPI command handler for the firmware
agent: agent
tools:
  - codebase
---

# Create SPI Command Handler

## Mission

Create a new SPI command handler in the firmware following existing patterns.

## Context

- See [driver.h](../../fw/src/driver.h) for the existing command interface
- Follow conventions in [firmware guidelines](../instructions/firmware.instructions.md)

## Inputs

The command name is: ${input:commandName:e.g. CMD_READ_STATUS}

If no command name is provided, ask the user before proceeding.

## Workflow

1. Add a new command enum value in the appropriate header
2. Implement the handler function following the existing pattern
3. Include parameter validation and error handling
4. Add a unit test in `fw/test/`

## Output Expectations

- One new `.c` / `.h` pair in `fw/src/` implementing the handler
- One new test file in `fw/test/` covering the handler

## Validation

- Build: `cmake --build --preset fw`
- Test: `ctest --preset fw`
```

## Testing and Iteration

- Open the prompt file in the editor and press the play button in the title bar to run it directly
- Choose whether to run in the current chat session or a new one
- Iterate on wording and context until the output is consistently useful
- Use the `chat.promptFilesRecommendations` setting to surface frequently used prompts when starting a new chat session

## Patterns to Avoid

- **Vague instructions**: "Make it good" gives the AI nothing actionable
- **Duplicated conventions**: Copy-pasting coding standards that belong in instruction files
- **Missing context**: Expecting the AI to know project structure without file references
- **Overloaded prompts**: Cramming multiple unrelated tasks into one prompt
- **Hardcoded paths**: Use variables (`${file}`, `${workspaceFolder}`) instead of absolute paths
- **Ignoring tool availability**: Referencing tools that do not exist (though unavailable tools are silently skipped, the prompt logic may still break)
- **No fallback for missing input**: Failing to specify what the AI should do when a required variable is empty
- **Overly broad tool lists**: Granting tools the prompt never uses increases surface area for unintended side effects

## Validation Checklist

Before committing a prompt file:

- [ ] Filename uses lowercase-with-hyphens and `.prompt.md` extension
- [ ] Frontmatter `description` clearly states what the prompt does
- [ ] Frontmatter `tools` list is minimal (least-privilege)
- [ ] Body starts with an `#` heading matching the prompt intent
- [ ] Body instructions are specific and actionable
- [ ] Body follows a logical flow: mission, context, inputs, workflow, output, validation
- [ ] File references use correct relative Markdown links
- [ ] Variables are used where the prompt should adapt to context
- [ ] Missing-input behavior is documented (defaults or fallback instructions)
- [ ] Output format, location, and success criteria are specified
- [ ] Destructive operations include guard rails or confirmation steps
- [ ] Shared conventions are linked (not duplicated) from instruction files
- [ ] Prompt tested via the editor play button with representative inputs

## Maintenance

- Version-control prompt files alongside the code they affect
- Update prompts when dependencies, tooling, or project conventions change
- Review tool lists and linked documents periodically to ensure they remain valid
- When a prompt proves broadly useful, extract shared guidance into instruction files to avoid duplication across prompts

## Additional Resources

- [Prompt Files Documentation](https://code.visualstudio.com/docs/copilot/customization/prompt-files#_prompt-file-format)
- [Awesome Copilot Prompt Files](https://github.com/github/awesome-copilot/tree/main/prompts)
- [Tool Configuration](https://code.visualstudio.com/docs/copilot/chat/chat-agent-mode#_agent-mode-tools)
