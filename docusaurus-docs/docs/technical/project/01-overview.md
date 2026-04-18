---
id: overview
title: Project Configuration
sidebar_label: Overview
description: Learn how to configure Zith projects using ZithProject.toml.
---

# Project Configuration

Zith uses `ZithProject.toml` for project configuration, similar to Cargo.toml in Rust or package.json in Node.js.

## Basic Structure

```toml
[project]
name = "my_project"
version = "0.1.0"
edition = "2024"
authors = ["Your Name <you@example.com>"]
description = "A brief description of your project"

[dependencies]
# External dependencies go here

[build]
# Build configuration

[profile.release]
# Release profile settings
```

## Sections

### [project]

Required project metadata:

- **name** - Project name (required)
- **version** - Semantic version (required)
- **edition** - Language edition (default: "2024")
- **authors** - List of authors
- **description** - Project description
- **license** - License identifier (e.g., "MIT", "Apache-2.0")
- **repository** - URL to source repository
- **homepage** - Project homepage URL

### [dependencies]

Specify external libraries your project needs:

```toml
[dependencies]
networking = "1.2.0"
graphics = { version = "2.0", features = ["vulkan"] }
local_lib = { path = "../local_lib" }
```

### [build]

Build configuration options:

```toml
[build]
target = "native"  # or specific architecture
optimize = true
debug_symbols = false
```

### [profile.*]

Define build profiles:

```toml
[profile.release]
optimize = "size"  # or "speed"
lto = true
strip_symbols = true

[profile.dev]
debug_symbols = true
optimize = false
```

## Example Configuration

```toml
[project]
name = "game_engine"
version = "0.2.0"
edition = "2024"
authors = ["GalaxyHaze <contact@galaxyhaze.dev>"]
description = "A high-performance 2D game engine"
license = "MIT"
repository = "https://github.com/galaxyhaze/zith-game-engine"

[dependencies]
renderer = "1.5.0"
audio = { version = "0.9", features = ["spatial"] }
utils = { path = "../utils" }

[build]
target = "x86_64-unknown-linux-gnu"
optimize = true

[profile.release]
optimize = "speed"
lto = true
strip_symbols = true

[profile.dev]
debug_symbols = true
incremental = true
```

## Environment Variables

Override configuration with environment variables:

- `ZITH_BUILD_TARGET` - Override build target
- `ZITH_OPTIMIZE` - Override optimization level
- `ZITH_DEBUG` - Enable debug mode

## Next Steps

- **[CLI Reference](../cli/overview.md)** - Build and manage projects
- **[Language Guide](../language/overview.md)** - Learn the language
- **[Raw & Unsafe](../raw/overview.md)** - Advanced features
