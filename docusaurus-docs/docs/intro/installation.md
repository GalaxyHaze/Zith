---
id: installation
title: Installing Zith
sidebar_label: Installation
description: Step-by-step guide to install Zith on Windows, macOS, and Linux.
---

# Installing Zith

Getting started with Zith is quick and easy. Choose your platform below:

## Quick Install (Recommended)

### Windows

```powershell
# Using PowerShell (admin privileges may be required)
iwr -useb https://zith-lang.org/install.ps1 | iex
```

Or download the installer from [GitHub Releases](https://github.com/galaxyhaze/Zith/releases).

### macOS

```bash
# Using Homebrew
brew install zith

# Or using the install script
curl -fsSL https://zith-lang.org/install.sh | bash
```

### Linux

```bash
# Using the install script
curl -fsSL https://zith-lang.org/install.sh | bash

# Or download from releases
wget https://github.com/galaxyhaze/Zith/releases/latest/download/zith-linux-x86_64.tar.gz
tar -xzf zith-linux-x86_64.tar.gz
sudo mv zith /usr/local/bin/
```

## Verify Installation

After installation, verify Zith is working:

```bash
zith --version
```

You should see output like:
```
Zith 0.1.0
```

## Manual Installation

### From Source

If you prefer to build from source:

```bash
git clone https://github.com/galaxyhaze/Zith.git
cd Zith
mkdir build && cd build
cmake ..
cmake --build . --config Release
sudo cmake --install .
```

**Requirements:**
- CMake 3.20+
- C++20 compatible compiler (GCC 11+, Clang 14+, MSVC 2022+)

## Configuration

### Environment Variables

Zith uses these optional environment variables:

- `ZITH_HOME` - Override installation directory
- `ZITH_EDITOR` - Default editor for `zith edit`
- `ZITH_CACHE_DIR` - Cache directory location

### Shell Completion

Add shell completions for better CLI experience:

```bash
# Bash
echo 'source <(zith completion bash)' >> ~/.bashrc

# Zsh
echo 'source <(zith completion zsh)' >> ~/.zshrc

# Fish
zith completion fish > ~/.config/fish/completions/zith.fish
```

## Next Steps

Now that Zith is installed:

1. **[Quick Start](./quickstart.md)** - Write your first Zith program
2. **[CLI Reference](../technical/cli/overview.md)** - Learn available commands
3. **[Language Guide](../technical/language/overview.md)** - Dive into syntax

---

**Having trouble?** Check our [FAQ](../technical/faq/overview.md) or open an issue on [GitHub](https://github.com/galaxyhaze/Zith/issues).
