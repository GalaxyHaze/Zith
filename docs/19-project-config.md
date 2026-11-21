## 19. Project Configuration

> **Status:** see [impl-status.md](impl-status.md) for the current status of project
> configuration and the `zithc create`/`zithc deps` commands.

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

[paths]
test_dir = "tests"        # default: tests; root for `zithc test` discovery

[dependencies]
std = "bundled"
```

`test_dir` is the root that `zithc test` walks to discover `*.zithm` files and
their `#[test]` functions. It defaults to `tests` when the key is absent, and
`zithc create` writes it into the generated `ZithProject.toml`.

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

### 19.3 Project Language Identity

> **Full-Zith design direction, not implemented:** a project identity will let
> a team enable or disable optional language features for its own codebase and
> attach a reason to disabled features. It does not redefine the universal
> public-API style or apply its choices to dependencies. The configuration
> syntax and diagnostic presentation remain open. See
> [ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md).

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
