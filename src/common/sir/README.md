# SIR Runtime

## Purpose

`src/common/sir` is a handwritten arena-backed intermediate representation used by compiler
frontend infrastructure and by the experimental codegen backend. It is not a generated helper; it
provides a small stable builder surface for modules, functions, types, values, variables, scopes,
and instructions.

## Architecture

`SirBuilder` owns an `Arena` and an internal `StringInterner`. `Module` owns interned names and a
`DynArray<Function *>`. `Function` owns parameter values, scopes, variables, values, and
instructions. `ScopeData` (`Scope`) owns variable declarations and value construction. All
pointers are arena-owned and stay valid until the arena is reset or destroyed.

The SIR opcode set is `Constant`, `Param`, `Add`, `Sub`, `Mul`, `Store`, and `Return`. Types are
declared through `Type` and the `Primitives` constants (`voidT`, `i1` through `i64`, `f32`, `f64`).

## Public API

```cpp
SirBuilder::SirBuilder(Arena &arena);
Module &SirBuilder::createModule(std::string_view name);
Function &Module::declareFn(std::string_view name, Type returnType, ArgsDecl args);
Function &Module::declareFn(std::string_view name);
Scope Function::pushScope();
Variable &Scope::declVar(std::string_view name, Type type);
Value &Function::param(size_t index);
Value &Function::add(Operand left, Operand right, Type resultType = {});
void Function::store(Variable &variable, Operand value);
void Function::ret(Operand value);
void Function::retVoid();
Result<void> verify(Module &module);
```

Operands accept `Value`, `Variable`, integral literals, or floating-point literals. Signatures and
parameter layouts are inferred from `ArgsDecl`; `verify` checks structure, ownership, scope, and
type consistency.

## Usage Flow

1. Create an `Arena` and a `SirBuilder`.
2. Call `createModule`, then `declareFn` for each function.
3. Push a scope, declare local variables, build values and stores, and add a return.
4. Run `toolkit::sir::verify` before lowering or consuming the module.

## Minimal Example

```cpp
Arena arena;
SirBuilder builder(arena);
auto &module = builder.createModule("sum-demo");
auto &function = module.declareFn(
    "sum", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
auto scope = function.pushScope();
auto &total = scope.declVar("total", Primitives::i32);
scope.store(total, function.add(function.param(0), function.param(1)));
function.ret(total);
auto result = toolkit::sir::verify(module);
```

## Dependencies

`zct_sir` links `zct_common` and exposes the project source and build include directories so
consumers can include `common/sir/sir.hpp`.

## Tests And Demo

- `sir-basics` (`tests/common/sir/sir-test.cpp`) covers building, verification, and scope
  behavior.
- `tests/common/sir/sir-demo.cpp` builds a verified `sum(i32, i32)` function with one local and
  prints module, function, parameter, and instruction counts.

```bash
cmake --build build --target sir-demo sir-test -j
ctest --test-dir build -R 'sir' --output-on-failure
```

## Agent Boundary

Keep `src/common/sir` focused on the arena-backed IR surface. It is handwritten runtime support;
new opcodes or instruction layout changes should first be checked against consumers such as
`src/codegen/`. Do not modify `src/symbols/`, `src/common/import/`, generators, or `tools/rules_kit/`
without explicit user approval.
