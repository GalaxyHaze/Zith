# SIR Runtime

## Purpose

`src/common/sir` is a handwritten arena-backed intermediate representation used by compiler
frontend infrastructure and by the experimental codegen backend. It is not a generated helper; it
provides a small stable builder surface for modules, functions, types, values, variables, scopes,
and instructions.

## Architecture

`SirBuilder` owns an `Arena` and an internal `StringInterner`. `Module` owns interned names and a
`DynArray<Function *>`. `Function` owns parameter values, scopes, variables, values, instructions,
and basic blocks. `ScopeData` (`Scope`) owns variable declarations and value construction, while
`BlockData` (`Block`) owns the values and terminator for one basic block. All pointers are
arena-owned and stay valid until the arena is reset or destroyed.

The separate `zct_sir_flat` library flattens a verified `Module` into an arena-backed
`toolkit::sir::flat::FlatModule`. Flat values use stable integer indices for types, variables,
operands, addresses, call arguments, blocks, and callees. Branch and return information stays on
`FlatBlock::terminator`, and the type table includes `Void`, `Bool`, `Char`, integer/float scalars,
arrays, slices, pointers, and user-defined names.

The SIR opcode set covers integer and float arithmetic (`Add`, `Sub`, `Mul`, `Div`, `Rem`),
bitwise and shift operations (`BitAnd`, `BitOr`, `BitXor`, `Shl`, `Shr`), comparisons (`Eq`, `Ne`,
`Lt`, `Le`, `Gt`, `Ge`), memory (`Load`, `Store`), calls (`Call`), and control flow (`Return`,
`Branch`, `CondBranch`). The value stream carries arithmetic/comparison/load/call values; branch
and return keep their information on the owning block's terminator. Types are declared through
`Type` and the `Primitives` constants (`voidT`, `i1` through `i64`, `f32`, `f64`).

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
Value &Function::sub(Operand left, Operand right);
Value &Function::mul(Operand left, Operand right);
Value &Function::div(Operand left, Operand right);
Value &Function::rem(Operand left, Operand right);
Value &Function::bitAnd(Operand left, Operand right);
Value &Function::bitOr(Operand left, Operand right);
Value &Function::bitXor(Operand left, Operand right);
Value &Function::shl(Operand left, Operand right);
Value &Function::shr(Operand left, Operand right);
Value &Function::eq(Operand left, Operand right);
Value &Function::ne(Operand left, Operand right);
Value &Function::lt(Operand left, Operand right);
Value &Function::le(Operand left, Operand right);
Value &Function::gt(Operand left, Operand right);
Value &Function::ge(Operand left, Operand right);
Value &Function::load(Variable &variable, Type resultType = {});
void Function::store(Variable &variable, Operand value);
Value &Function::call(Function &callee, std::span<const Operand> arguments);
Block Function::pushBlock();
void Function::selectBlock(Block &block);
void Function::br(Block &target);
void Function::condBranch(Operand condition, Block &trueTarget, Block &falseTarget);
void Function::ret(Operand value);
void Function::retVoid();
Result<void> verify(Module &module);
```

Operands accept `Value`, `Variable`, integral literals, or floating-point literals. Signatures and
parameter layouts are inferred from `ArgsDecl`; `verify` checks structure, ownership, scope, and
type consistency. It also rejects missing branch targets, foreign blocks or values, unsupported
load/store widths, immutable stores, call arity/type mismatches, and unresolved calls.

## Usage Flow

1. Create an `Arena` and a `SirBuilder`.
2. Call `createModule`, then `declareFn` for each function.
3. Push a scope, declare local variables, build values, stores, calls, and blocks as needed,
   and terminate each block with `ret`, `br`, or `condBranch`.
4. Run `toolkit::sir::verify` before lowering or consuming the module.

## Flat And Serialization

Flattening is a lowering boundary for future backends that consume integer-indexed IR instead of
arena pointer graphs. `flattenModule` calls the SIR verifier first, interns every used type, and
preserves source block order so `baseBlock`, branch targets, and terminator fields remain stable.

```cpp
auto flat = toolkit::sir::flat::flattenModule(module);
auto bytes = toolkit::sir::flat::serializeFlatModule(flat.value());

Arena roundtripArena;
auto decoded = toolkit::sir::flat::deserializeFlatModule(roundtripArena, bytesView);
```

The `.sir` byte stream is deterministic little-endian and carries a `ZCTSF1` magic/version header.
Names are serialized as strings and re-interned into the destination arena, so round-tripping does
not depend on the source `StringInterner` layout. Deserialization verifies bounds, known opcodes,
terminator shape, type indices, and trailing bytes before returning a `FlatModule`.

The flat library currently lowers and serializes the full SIR opcode surface; execution/lowering
of the new memory, call, and control-flow ops remains future work.

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

## Block Example

```cpp
Arena arena;
SirBuilder builder(arena);
auto &module = builder.createModule("branch-demo");
auto &fn = module.declareFn(
    "pick", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});

auto thenBlock = fn.pushBlock();
auto elseBlock = fn.pushBlock();
auto join = fn.pushBlock();

Block entry{fn.baseBlock};
fn.selectBlock(entry);
fn.condBranch(fn.lt(fn.param(0), fn.param(1)), thenBlock, elseBlock);

fn.selectBlock(thenBlock);
fn.ret(1);

fn.selectBlock(elseBlock);
fn.ret(2);

fn.selectBlock(join);
fn.ret(0);
```

`pushBlock` creates a new block and makes it current. `selectBlock` switches construction back to
an existing block. Every builder appends values to the function's current block, so builders
should switch blocks before emitting terminators or block-local values.

## Future Work

Codegen lowering for `Div`, `Rem`, bitwise/shift/comparison, load/store, call, and
branch/condBranch is future work. `src/codegen/` currently rejects these opcodes instead of
fabricating a lowering; this SIR expansion is an IR and builder deliverable, not a new backend
execution path.

## Dependencies

`zct_sir` links `zct_common` and exposes the project source and build include directories so
consumers can include `common/sir/sir.hpp`.

## Tests And Demo

- `sir-basics` (`tests/common/sir/sir-test.cpp`) covers building, verification, scope, memory,
  call, block, and branch behavior.
- `sir-flat-basics` (`tests/common/sir/sir-flat-test.cpp`) covers flat opcode preservation,
  memory/call topology, branch terminators, structural type round-trips, and malformed streams.
- `sir-flat-demo` writes a scalar module through the binary round-trip and reports counts.
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
