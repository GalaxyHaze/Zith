---
id: getting-started-installation
title: Installation
section: Getting Started
output: getting-started/D-installation.html
aliases: intro/D-installation.html
kind: editorial
---
# Installation

Build `zithc` from the compiler checkout. Packaging instructions are intentionally not published here because package availability is not part of the canonical project documentation.

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
cmake -S . -B build
cmake --build build
```

Run the resulting binary directly, or add its build directory to your `PATH`.

```bash
./build/zithc --help
```

Use [Quick Start](doc:getting-started-quick-start) once the compiler is available.
