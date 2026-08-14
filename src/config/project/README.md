# Project Config Helper

# Project Scaffold

## Purpose

`project/` contains the project-tree scaffold blueprints. The typed `ProjectConfig` defaults now
live in `src/config/flags/default.toml`.

## Source Of Truth

Edit `scaffold.toml` to change generated project files. The scaffold is a manual tool and is never
invoked as a CMake build target.

## Run

```bash
python3 src/config/project/scaffold.py \
  --rules src/config/project/scaffold.toml \
  --out .
```

## Agent Boundary

Edit `scaffold.toml` to change project-tree blueprints. Do not edit generated project files
manually. The scaffold is a manual tool and is never invoked as a CMake build target; do not
modify `scaffold.py` or shared generator rules without explicit user approval. Running the script
to create or refresh a project tree is allowed.

## Demo

The config demo lives at `tests/config/config-demo.cpp`; build it with the project target
`config-demo` and run it as `config-demo`. The generated config API is documented in
`src/config/README.md`.
