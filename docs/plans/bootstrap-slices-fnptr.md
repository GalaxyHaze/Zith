# Function values and C-runtime slices

## Summary

This bootstrap step makes `fn(params): R` usable as a first-class value type and
keeps the existing C ABI representation for mutable slices. It does not add
closures, generic function-pointer types, or automatic slice memory management.

## Frontend

`fn(i32): i32` is parsed as a `TypeExprKind::Function` node. Its
`arguments` vector stores each parameter first and the return type last, which
matches the existing sema function lowering and compact type encoding. A return
type is required after `:`; `->` in a type value is not accepted. The formatter
and C API render the same `fn(params): R` spelling.

## Sema and HIR

Normal and `extern fn` declarations already lower to `FunctionType`, so
`var f: fn(i32): i32 = double;` and `apply(f, 7)` pass semantic analysis once
the parser accepts function types. HIR lowering tags indirect calls with a
lowered `HirCall::fn_type`, while direct calls keep `callee = kInvalidHirExpr`
and a resolved symbol.

## Codegen

LLVM lowers `TypeFn` values to pointer type, matching the C function-pointer
ABI. The call emitter reconstructs a concrete `llvm::FunctionType` from
`HirCall::fn_type` for indirect calls and invokes the callee value through it.
Direct calls continue to resolve through the HIR symbol table.

## Cache and verification

`CompactExprKind::Call` serializes `fn_type` in `ref_e`, alongside the existing
`callee` and `resolved_fn` fields. Tests cover parsing, sema acceptance and
type-mismatch rejection, HIR shape, function-pointer codegen against a C
runtime, mutable slice reads/writes against a C runtime, and cache/ZIRL
round-trips for function and slice compact types.

## Example artifacts

`examples/bootstrap-slices-fnptr.zith` and its C helper show a C-provided
slice and function value in one program. The automated example suite only runs
self-contained `.zith` programs, so the codegen test links the equivalent C
runtime directly and also covers cold and warm cache paths with the same
compact fields.
