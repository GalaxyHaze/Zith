# Defer and Drop Proposal (Historical)

Status: superseded by ADR-0055

## Objective

`defer expr;` is implemented as an explicit scope cleanup statement in the Zig
style: expressions registered inside a block run when that block exits, in
reverse registration order, even across `return`, `break`, `continue`,
`jump`, and normal fallthrough.

`drop` was considered as a deterministic owner cleanup hook. ADR-0055 rejects
that feature for Zith--. The keyword may remain reserved, but owner cleanup
stays explicit.

This file preserves the implementation record for `defer` and the rejected
`drop` proposal. It is not an active implementation roadmap.

## Scope Split

The `main` target remains `Zith--` as defined by `docs/Zith--.md`. `defer` is
part of Zith-- with parser, sema, HIR, codegen, and test coverage. Automatic
per-owner `drop` cleanup is not part of the language design:

| Surface | Status |
| --- | --- |
| `defer expr;` / `defer { ... }` | Working; reverse-order block cleanup |
| `defer` in `state` bodies | Working; cleanup runs before `jump` |
| `drop` keyword | Reserved only; automatic cleanup is rejected by ADR-0055 |
| `docs/impl-status.md` | `defer` is `Working`; `drop` is not planned for Zith-- |

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

`defer` statements in macro templates are cloned into the expanded code and
follow the normal sema path. A `raw macro` that expands to `defer` currently
registers cleanup at the expansion boundary rather than the surrounding
caller block. The proposed caller-block behavior for variadic cleanup macros
is tracked in [the variadic macro plan](variadic-macros-and-defer.md).

### Rejected `drop` proposal

The following syntax was considered and rejected. It is preserved here as
historical context, not as a Zith-- proposal:

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
5. Type-check `drop` as a method and add ownership diagnostics. Rejected by
   ADR-0055.
6. Add `drop` tests and update the language contract. Rejected by ADR-0055.

## Verification

```bash
cmake --build build -j
./build/zithc --include stdlib check build/main.zith
ctest --test-dir build --output-on-failure
cmake --build build --target fmt-check
```

If LLVM is unavailable, parser/sema/HIR tests must still run and the final
report must state that codegen was not verified.
