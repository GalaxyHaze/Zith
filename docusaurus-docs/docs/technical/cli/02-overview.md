---
id: check-command
title: Check Command
sidebar_label: check
description: Analyze Zith code for errors without building. Fast feedback during development.
---

# `zith check`

The `check` command analyzes your code for errors without generating a binary. Perfect for quick feedback during development.

## Usage

```bash
zith check [options] [file|project]
```

## Examples

### Check Current Project

```bash
# Check entire project from root directory
zith check
```

### Check Specific File

```bash
# Check a single file
zith check src/main.zith

# Check with line numbers
zith check --verbose src/module.zith
```

### Check All Files in Directory

```bash
# Recursively check all .zith files
zith check ./src/
```

## Options

| Flag | Description |
|------|-------------|
| `--verbose, -v` | Show detailed error information |
| `--warnings-as-errors` | Treat warnings as errors |
| `--no-color` | Disable colored output |
| `--json` | Output errors in JSON format |
| `--quiet, -q` | Only show errors, no warnings |

## Output Format

### Default Output

```
Checking src/main.zith...
error[E001]: Type mismatch
  --> src/main.zith:15:8
   |
15 | let x: i32 = "hello";
   |         ^^^   ^^^^^^ expected i32, found str
   |
   = help: Change type to 'str' or value to an integer
```

### JSON Output

```bash
zith check --json
```

```json
{
  "file": "src/main.zith",
  "line": 15,
  "column": 8,
  "level": "error",
  "code": "E001",
  "message": "Type mismatch",
  "help": "Change type to 'str' or value to an integer"
}
```

## Exit Codes

- `0` - No errors found
- `1` - Errors found
- `2` - Fatal error (file not found, etc.)

## Common Error Codes

| Code | Description |
|------|-------------|
| E001 | Type mismatch |
| E002 | Undefined variable |
| E003 | Duplicate definition |
| E004 | Missing return statement |
| E005 | Invalid ownership modifier |
| W001 | Unused variable (warning) |
| W002 | Unreachable code (warning) |

## Integration with Editors

Use `--json` flag for editor integration:

```bash
# VS Code task example
{
  "label": "Zith Check",
  "type": "shell",
  "command": "zith check --json",
  "problemMatcher": {
    "owner": "zith",
    "pattern": {
      "file": "${file}",
      "line": "${line}",
      "message": "${message}"
    }
  }
}
```

## Tips

- Run `zith check` before committing to catch errors early
- Use `--warnings-as-errors` in CI/CD pipelines
- Combine with `zith fmt` for clean, error-free code

## See Also

- [`zith build`](./build.md) - Build the project
- [`zith fmt`](./fmt.md) - Format code
- [`zith run`](./run.md) - Run program directly
