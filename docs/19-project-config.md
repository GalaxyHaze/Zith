## 19. Project Configuration

> **Implementation status:** `ZithProject.toml` project scaffolding is **working** via
> `zithc create`. Dependency management (`zithc deps`) is a **stub**.
> See [impl-status.md](impl-status.md).

### 19.1 `ZithProject.toml` (per-project)

```toml
[project]
name    = "my_app"
version = "0.1.0"

[build]
runtime = true            # default: true; set false for OS/embedded targets
asm     = "x86_64_intel"  # required if using inline assembly
                          # errors if it diverges from the host machine or other project files

[assets]
paths = ["assets/"]       # compile-time-validated asset paths

[dependencies]
std = "bundled"
```

### 19.2 `ZithFlags` (compiler / global)

```
--runtime=false     # disable the runtime globally: removes most allocators,
                    # disables `must`, disables dynamically linked libraries and
                    # anything that depends on them, and forces all available
                    # std to be statically linked
--asm=arm64         # set the assembly dialect globally
--release           # release mode 
--debug             # debug mode (default)
```

> `runtime = false` disables the heap, standard stack assumptions, and signal handlers. Any standard library feature that requires a runtime becomes unavailable at compile time.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
