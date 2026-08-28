---
id: getting-started-installation
title: Installation
section: Getting Started
output: getting-started/D-installation.html
aliases: intro/D-installation.html
kind: editorial
---
# Installation

Zith provides package-manager installs, fetch installers, and a source build. The compiler
currently works on Linux, macOS, and Windows.

## Homebrew

```bash
brew tap galaxyhaze/zithc
brew install zithc
```

## Scoop

```powershell
scoop bucket add zithc https://github.com/GalaxyHaze/Zith.git
scoop install zithc
```

## Fetch installer

On Linux or macOS:

```bash
curl -fsSL https://raw.githubusercontent.com/GalaxyHaze/Zith/main/scripts/install.sh | sh
```

Use `--musl` to install the static musl-linked binary when dynamic linking is not suitable.

On Windows PowerShell:

```powershell
Invoke-RestMethod https://raw.githubusercontent.com/GalaxyHaze/Zith/main/scripts/install.ps1 | Invoke-Expression
```

## Build from source

Required toolchain versions are documented in the [compiler README](https://github.com/GalaxyHaze/Zith#readme).

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
cmake -S . -B build
cmake --build build
```

```bash
./build/zithc --help
```

Run `./build/zithc --help` after the build finishes, or add the build directory to `PATH`.
LLVM 18+ is optional but required for LLVM code generation; without it, `zithc check` remains
available. Use [Quick Start](doc:getting-started-quick-start) once the compiler is available.
