# Session Helper

## Purpose

The session helper generates a reusable compilation-pipeline driver from `session.rules`. It is no
longer tied to one fixed compiler-construction layout: the generated `CompilationSession` owns the
pipeline mechanics and any truly session-local state, while domain services live in a single
user-owned context object.

The session is a concrete generated type. It still emits `CompilationSession`, `Stage`,
`PipelinePlan`, `StageError`, `StageResult`, `dispatch<Stage>()`, `run()`, `runTo()`,
`resume()`, and per-stage result storage.

## Files

| File | Responsibility |
|---|---|
| `session.rules` | Declares the injected context type, stages, stage outputs, and optional session-local state. |
| `generate.py` | Reads `session.rules` and emits the session C++ surface. |
| `types.hpp` | User-owned include surface for the context type and non-built-in output types. |
| `dispatch.cpp` | Hand-written `dispatch<Stage>()` implementations. |
| `CMakeLists.txt` | Builds the generated core and the production dispatch library. |
| `build/src/session/session.hpp` | Generated public session API. |
| `build/src/session/session.cpp` | Generated pipeline implementation. |
| `build/src/session/dispatch.hpp` | Generated dispatch declarations and result aliases. |

## Rules Syntax

### `[context]`

Exactly one context type must be declared. It is injected by reference into the generated session.

```text
[context]
type: my::CompilerContext
```

The context type must be visible through `src/session/types.hpp` or the include surface pulled in
by it.

### `[stages]`

Stages are ordered pipeline phases. Each stage may declare an output type; `void` is used when the
stage has no result value.

```text
[stages]
Source: void
Lexed: generated_lexer::TokenStream
Scanned: void
```

The first stage is the initial pipeline entry and the last stage is the `run()` target.

### `[state]`

Optional session-local fields owned by the generated session. These are not compiler services;
compiler-owned services belong in the context type.

```text
[state]
attempts: int = 0
```

The default expression is emitted as part of the constructor initializer.

## Generated API

```cpp
my::CompilerContext context;
zith::session::CompilationSession session(context);

const auto result = session.runTo(zith::session::Stage::Lexed);
if (!result) {
    // result.error().stage identifies the failing stage.
    return false;
}
```

Pipeline execution stores each successful stage output in the session. Query or read a stored
result by stage:

```cpp
if (session.hasStageResult<zith::session::Stage::Lexed>()) {
    auto &tokens = session.stageResult<zith::session::Stage::Lexed>().value();
    // tokens is generated_lexer::TokenStream for this stage's rule output.
}
```

A later stage reads an earlier stage's stored output directly from the session. There is no
automatic result parameter passed between phases:

```cpp
template <>
common::memory::Result<ScannedResult>
dispatch<Stage::Scanned>(CompilationSession &session) {
    if (!session.hasStageResult<Stage::Lexed>()) {
        return common::memory::Error{"scanner requires a successful Lexed stage"};
    }
    const auto &tokens = session.stageResult<Stage::Lexed>().value();
    return tokens.empty()
        ? common::memory::Error{"cannot scan an empty token stream"}
        : common::memory::Result<ScannedResult>{};
}
```

This keeps the generated dispatch signature unchanged: each `dispatch<Stage>()` receives only
`CompilationSession &`, and the session is the indexed storage for every prior successful result.
`void` stages also store a successful result, so `runTo()` exposes ordered stage progress even
before a real AST/IR helper exists. Direct `dispatch<Stage>()` calls do not write these slots; only
`run()`, `runTo()`, and `resume()` do.

The storage invariants are:

- A stage result exists only after the pipeline stored that stage's successful dispatch output.
- Calling `dispatch<Stage>()` directly never populates stage result slots.
- `run()`, `runTo()`, and `resume()` are the paths that dispatch stages and store their results.

Important generated members:

| Member | Behavior |
|---|---|
| `CompilationSession(Context &)` | Injects the user-owned context by reference. |
| `context()` | Returns mutable or const access to the injected context. |
| `diags()` | Session-local diagnostic vector. |
| `run()` | Runs to the final declared stage. |
| `runTo(Stage)` | Runs from the first stage through the requested target. |
| `resume()` | Continues from `plan.current` toward `plan.target`. |
| `hasStageResult<Stage>()` | True when the pipeline stored a successful result for a stage. |
| `stageResult<Stage>()` | Returns the stored `Result<Output>` for a stage. |
| `hasErrors()` | True if the session has recorded errors. |

## Handwritten Dispatch

`dispatch.cpp` is the handwritten specialization surface. Each stage needs a
`dispatch<Stage::X>(CompilationSession &)` implementation linked into the production target.

Use `session.context()` for domain state:

```cpp
template <>
common::memory::Result<LexedResult>
dispatch<Stage::Lexed>(CompilationSession &session) {
    auto &context = session.context();
    if (context.sourceFile.empty())
        return common::memory::Error{"missing source"};
    return lex(context.sourceFile, context.interner);
}
```

The generated runtime dispatches the non-template `dispatch(Stage, CompilationSession &)` to the
template specializations.

## CMake Targets

| Target | Contents |
|---|---|
| `zith_session_core` | Generated pipeline source only; usable by tests that provide their own dispatch bodies. |
| `zith_session` | Generated pipeline and handwritten `dispatch.cpp`; used by production executables. |

Static libraries commonly require `zith_session` callers to include the handwritten dispatch
implementation. The session wiring avoids a link-order workaround by sharing one generated object
between both targets.

## Common Workflow

1. Read `src/session/session.rules` and `src/session/types.hpp` before editing.
2. Edit `session.rules` to change the context type, stage list, or stage outputs.
3. Update `types.hpp` if a new context type or required include surface is needed.
4. Regenerate:

```bash
python3 src/session/generate.py src/session/session.rules --out build/src/session
```

5. Update `dispatch.cpp` if stage behavior changed.
6. Rebuild and run the tests:

```bash
cmake --build build -j
ctest --test-dir build -R session --output-on-failure
```

## Tests

- `session-generated-basics` constructs a `ZithSessionContext`, applies the generated pipeline, and
  checks `runTo`, failure, and `resume()` behavior.
- `session-generator-regression` verifies the generator accepts one `[context]`, rejects missing or
  duplicate contexts, rejects invalid type syntax, and compiles a smoke consumer with custom
  dispatch bodies.

## Agent Boundary

Edit `session.rules` for context/stage/state declarations, `types.hpp` for user-owned include
surface, and `dispatch.cpp` for pipeline behavior. Do not edit `build/src/session/*`. Do not
modify `generate.py` or shared generator rules without explicit user approval.
