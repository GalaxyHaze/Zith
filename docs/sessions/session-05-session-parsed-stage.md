# Session 05 - Wire the `Parsed` Session Stage and CLI Check

## Context

The session pipeline has `Lexed` implemented and later semantic stages remain
stubs. This session installs a real `Parsed` stage immediately after `Lexed`,
makes `zithc check` stop at that stage, and verifies parser diagnostics plus the
stored `ParseOutput`.

The handwritten parser entry point from Session 03 is:

```cpp
hooks::parser::parseSource(parser, tokens, source)
```

## Goal

Insert a real `Parsed` stage immediately after `Lexed`, make session dispatch run
the generated lexer stream through the handwritten parser, propagate parser
diagnostics into the session diagnostics vector, and make `zithc check` stop at
`Parsed`.

## Out of Scope

- Sema/import/resolution stages remain stubs.
- Macro expansion and symbols remain stubs.
- `build` and `run` do not need real later stages in this session.

## Files You May Edit

- `src/session/session.rules`
- `src/session/types.hpp`
- `src/session/dispatch.cpp`
- `src/session/CMakeLists.txt`
- `src/cli/handlers.cpp`
- `tests/session/CMakeLists.txt`
- `tests/session/session-test.cpp` or a new session parse-stage test

## Files You May Read

- `build/src/session/session.hpp`
- `build/src/session/dispatch.hpp`
- `build/src/session/session.cpp`
- `src/frontend/parser/parse.hpp`

## Steps

1. Edit `src/session/session.rules`. Add the real `Parsed` stage immediately
   after `Lexed`:
   ```text
   Source: void
   Lexed: generated_lexer::TokenStream
   Parsed: sample::ParseOutput
   Scanned: void
   Imported: void
   Resolved: void
   TypeChecked: void
   Solved: void
   NraResolved: void
   HirLowered: void
   CodegenReady: void
   Cached: void
   ```
2. Edit `src/session/types.hpp`:
   - Include `frontend/parser/types.hpp` so `sample::ParseOutput` is visible to
     generated session code.
   - Optionally reserve ownership of parser/recovery state inside
     `ZithSessionContext` if Session 03 introduced a parser context state type.
3. Regenerate session:
   ```bash
   python3 src/session/generate.py src/session/session.rules --out build/src/session
   ```
4. Edit `src/session/dispatch.cpp`:
   - Add includes for `frontend/parser/parse.hpp`.
   - Implement `dispatch<Stage::Parsed>`:
     ```cpp
     template <>
     common::memory::Result<ParsedResult> dispatch<Stage::Parsed>(CompilationSession &session) {
         auto &context = session.context();
         const auto loc = context.sourceMap.get(context.fileId);
         if (!loc)
             return common::memory::Error{"Parsed: missing source"};

     auto &lexed = session.stageResult<Stage::Lexed>().value();
     generated_parser::Parser<sample::ParseOutput> parser(context.arena);
     sample::ParseOutput output =
         hooks::parser::parseSource(parser, tokens, loc->get().slice());

         for (const auto &diagnostic : output.diagnostics) {
             session.diags().push(Diagnostic{
                 .span = common::memory::SourceSpan{context.fileId, diagnostic.span},
                 .message = diagnostic.message,
             });
         }
         return output;
     }
     ```
   - Keep all later stage specializations as existing stubs.
5. Update `src/session/CMakeLists.txt`:
   - Add parser/ast include directories to `zct_session_core_objects` and
     `zct_session`.
   - Link `zct_frontend_parser` (which should transitively pull
     `zct_frontend_lexer`, `zct_frontend_ast`, and `zct_common`).
6. Update `src/cli/handlers.cpp`:
   - Change `zithc check` from `Stage::TypeChecked` to `Stage::Parsed`.
   - Keep build/run targets and help text behavior unchanged unless a test
     intentionally requires them to remain at later stubs.
7. Add a session parse-stage test:
   - Create a `ZithSessionContext`.
   - Add source through `sourceMap.addFile`.
   - Run `CompilationSession.runTo(Stage::Parsed)`.
   - Assert `hasStageResult<Stage::Parsed>()` and that the stored
     `ParsedResult` is a `sample::ParseOutput` with a `Program` root.
   - Feed malformed source and assert session diagnostics carry the parser
     span.
8. Link the new test if it is a separate target in `tests/session/CMakeLists.txt`.

## Verification Steps

```bash
cmake --build build --target zct_session -j
ctest --test-dir build -R '^(session|parsed)' --output-on-failure
```

Then run a manual CLI check:

```bash
cat > /tmp/zith-parity-check.zith <<'EOF'
pub fn main() -> i32 {
    return 0;
}
EOF
./build/zithc/zithc check /tmp/zith-parity-check.zith
```

With verbose output it should reach `Parsed`.

## Implementation Notes

The real dispatch re-tokenizes from the session source map, constructs the
generated parser over the session arena, and calls the token-aware
`parseSource(parser, tokens, source)`. `ParseOutput` is move-only and reparents
its internal `AstRoot` arena during moves, so the session can store the result
safely without invalidating AST nodes. Parse diagnostics are copied into
`session.diags()` with the session file id and the parser span.

```bash
./build/zithc/zithc --verbose check /tmp/zith-parity-check.zith
```

## Acceptance Criteria

- `zithc check <valid_main_style_file>` reaches `Parsed`.
- Parse diagnostics propagate to `session.diags()` with source file and span.
- `Parsed` output is stored as `sample::ParseOutput` in the session result slot.
- Later stages remain deliberate stubs.
- Focused session tests pass.

## Expected Next State

Session 06 can build the full tree and update the migration matrix.
