# Step 01 — Trait And Interface Bodies

## Goal

After this step, `trait` and `interface` declarations store their members in the
frontend snapshot instead of being skipped. Trait method requirements and default
methods, and interface field lists, are visible to sema, the formatter and the
cache path.

## Prerequisites

None. This is the first step.

## Baseline Facts

- `lowerDeclaration` falls through to `skipDelimited('{', '}')` for any remaining
  declaration body at [frontend.cpp](/home/diogo/Zith/src/frontend/frontend.cpp:2458).
- `trait Pr { fn print(self); }` today passes `zithc check` with no semantic
  member, and `implement P as Pr` consumes the trait name without storing it.
- The formatter already has a `DeclKind::Trait` case but emits the entire body
  token span verbatim through `emitNominalDecl`, so no member-specific formatting
  exists yet.
- `frontend-printer.cpp` prints nested method lists from `decl.parameters`, not
  from separate member declarations, so trait members need a deliberate shape.

## Design Contract

1. `trait Name { ... }` is parsed as a declaration with:
   - zero or more nested `DeclKind::Function` declarations,
   - each function may have a body (default method) or no body (requirement),
   - generic parameters remain supported,
   - `ownerName` is set to the trait name for all nested functions.
2. `interface Name { ... }` is parsed with:
   - zero or more field declarations using the grouped `[x, y]: T` syntax,
   - no nested `fn` declarations,
   - a dedicated `InterfaceMethodNotAllowed` diagnostic if `fn` appears.
3. `Declaration` already has enough storage: use `parameters` for interface fields
   and `ownerName` for trait methods. Do not add a new AST node kind.
4. The parser must not discard `as Trait` / `for Trait` in `implement` blocks; that
   value is needed by step-03. This step only preserves it.
5. Formatter emits trait/interface member declarations from the snapshot rather
   than the original span when no comment forces `emitOriginal`.

## Implementation Steps

1. Add a `traitName` field to `frontend::Declaration` (empty unless written
   `implement Owner as Trait` or `implement Owner for Trait`).
2. In `lowerImplementBlock`, store the optional trait name and pass it to
   `lowerDeclaration` for method declarations.
3. Split the body-lowering fallback in `lowerDeclaration` so `DeclKind::Trait`
   parses members and `DeclKind::Interface` parses fields, instead of
   `skipDelimited`.
4. Reuse the grouped-field parser from `DeclKind::Struct` for interface bodies; a
   shared private helper is preferred over copy-paste.
5. Add formatter branches for trait and interface members, using
   `decl.ownerName == decl.name` to locate nested methods.
6. Extend `frontend-printer.cpp` so member functions and interface fields are
   visible in `--ast` output.
7. Keep the cache path able to serialize the new fields without changing the
   artifact format unless required; verify round-trip manually and record the
   exact cache records that moved.

## Diagnostics

Add only:

- `InterfaceMethodNotAllowed` (`E2025`, value `2025`): an interface body contains
  `fn`.
- `NotATrait` (`E2023`, value `2023`): `implement T as X` where `X` is not a trait.

If a name is needed in the implementation before semantic checking exists, use
`NotImplemented` (`E2009`) with a clear message, but replace it in step-03.

## Verification

Create `tests/test-trait-parser.cpp` and register it with `add_zith_test`. The
test may follow the existing pattern of `tests/test-frontend.cpp` or use the
frontend public API `zith::frontend::parse(source)`, inspecting
`snapshot.declarations()` and `snapshot.diagnostics()`. Do not invent private
accessors beyond what is already public.

Source under test:

```zith
trait Printable {
    fn print(self);
    fn describe(self): i32 { return 1; }
}
interface Positioned { [x, y]: f32 }
```

Assertions: no diagnostics; `Printable` has two nested functions with
`ownerName == "Printable"`; `Positioned` has one grouped field entry.

Negative:

```zith
interface Bad { fn method(self); }
```

Must emit exactly `E2025`.

Commands:

```bash
cmake --build build -j --target test-trait-parser
./build/tests/test-trait-parser
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: move `trait`/`interface` parse status from Working to a new
  row "trait/interface member storage (parser)" as completed at parse stage.
- `docs/04-traits-interfaces.md` implementation-status header: note that bodies
  are parsed and conformance arrives in step-03.
- `docs/roadmap.md`: mark F-35 parser half complete, semantic half pending
  step-03.
