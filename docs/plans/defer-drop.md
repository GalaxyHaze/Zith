# Defer and Drop Roadmap

## Objective

`defer expr;` is implemented as an explicit scope cleanup statement in the Zig
style: expressions registered inside a block run when that block exits, in
reverse registration order, even across `return`, `break`, `continue`,
`jump`, and normal fallthrough.

`drop` is intentionally narrower: it does not exist yet as a semantic feature,
only as a reserved keyword in the modern lexer and `TokenKind::Drop` in the
legacy lexer. The candidate semantics are "run the drop hook when a value exits
its scope". This roadmap keeps `drop` behind `defer` so both are implemented as
one scoped-cleanup contract instead of two unrelated features.

This plan tracks the deferred scope-cleanup contract. `defer` is implemented
in this iteration; `drop` remains a reserved keyword and a roadmap candidate.

## Scope Split

The `main` target remains `Zith--` as defined by `docs/Zith--.md`. `defer` is
now part of `Zith--` with parser, sema, HIR, codegen, and test coverage.
`drop` remains outside the subset until ownership cleanup is implemented:

| Surface | Status |
| --- | --- |
| `defer expr;` / `defer { ... }` | Working; reverse-order block cleanup |
| `defer` in `state` bodies | Working; cleanup runs before `jump` |
| `drop` keyword | Reserved only; no parser branch consumes it |
| `docs/impl-status.md` | `defer` recorded as `Working`; `drop` remains `Spec only` |

## Minimal Semantics

### `defer`

```zith
fn closeFile(f: ?*FILE) {
    defer fclose(f);
    if (f is null) {
        return;
    }
    // ...
}
```

Rules:

- `defer expr;` is a statement, not an expression.
- Expressions are evaluated when the enclosing block exits, in reverse order.
- `defer { ... }` registers the whole body as a cleanup-only block that does
  not produce a value; statements in it run in written order when the block
  exits.
- `return value;` evaluates the returned value before any deferred body.
- A deferred expression may not be a `jump`, `return`, `break`, or `continue`.
- The body can call functions and mutate captured locals through `lend`/`var`
  exactly like ordinary code after scope analysis.
- Sema records the deferred expression on the nearest lexical block; HIR
  lowering appends the cleanup sequence before that block's terminator.

`defer` is rejected inside macro templates by the parser's clean control-flow
diagnostic; ordinary macros whose expanded bodies contain `defer` follow the
normal parser/sema path.

### `drop`

Candidate syntax, to be confirmed before implementation:

```zith
struct Buffer {
    ptr: *u8,
    len: u64,

    fn drop(var self) {
        free(self.ptr);
    }
}
```

Rules:

- `drop` starts as the name of a deterministic method called when the value's
  scope ends, not as an operator expression.
- Only one `drop` per owner type is allowed; it is invoked via the same
  reverse-order scope cleanup used by `defer`.
- `drop` must integrate with NRA residual facts: no use-after-drop, no
  double-drop through views/borrows, and no dropping behind `view`.
- Until the full NRA proof exists, the safe implementation is to reject `drop`
  in paths where ownership cannot be proven statically.

## Work Items

1. Add `defer` to the modern keyword table and `StmtKind::Defer`; parse
   `defer expr;` and `defer { ... }`. Done.
2. Add sema scope handling: register in reverse, reject control-flow expressions
   inside a deferred body, and verify captures against their binding lifetime.
   Done.
3. Add HIR lowering: a block-level cleanup instruction emitted before every
   terminator of the block. Done.
4. Add codegen coverage for normal exit, `return`, `break`, `continue`, and
   `jump` in `state` functions. Done.
5. Type-check `drop` as a method, add the owner hook to HIR cleanup, and add
   ownership diagnostics for double drop / drop from a view. Pending.
6. Add tests under `tests/test-defer.cpp` and `tests/test-drop.cpp`, registered
   with `add_zith_test`, and update `docs/Zith--.md`,
   `docs/Zith---implementation.md`, `docs/impl-status.md`, and
   `docs/09-control-flow.md`. Defer tests are covered in the existing focused
   suites and docs are updated; `drop` tests remain pending.

## Verification

```bash
cmake --build build -j
./build/zithc --include stdlib check build/main.zith
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

If LLVM is unavailable, parser/sema/HIR tests must still run and the final
report must state that codegen was not verified.
