# ZCT Type-System Helper

The type-system helper provides a small, declarative type catalogue plus a
runtime registry for custom composite types and operator rules. It is an
experimental standalone library and does not replace the compiler's SIR
`Type` representation.

## Layout

- `type-system.rules` is the declarative source of truth for builtin types,
  coercions, casts, and common-type rows.
- `generate.py` emits `build/src/common/type-system/type-system-table.hpp`.
- `type-system.hpp` and `type-system.cpp` implement `TypeContext`, which
  combines the generated tables with runtime custom entries.
- `tests/common/type-system/` contains the C++ basics test and the generator
  regression test.

## Rules Syntax

```text
[metadata]
name = "toolkit-types"
version = 1

[types]
void    : primitive
bool    : primitive
char    : primitive
i1      : integer bits=1 signed
i8      : integer bits=8 signed
i16     : integer bits=16 signed
i32     : integer bits=32 signed
i64     : integer bits=64 signed
u8      : integer bits=8
u16     : integer bits=16
u32     : integer bits=32
u64     : integer bits=64
f32     : float bits=32
f64     : float bits=64
string  : opaque
ptr     : pointer
array   : array
slice   : slice
fn      : function
opt     : optional
struct  : nominal
enum    : nominal
union   : nominal
userdef : userdef

[coercions]
i8  -> i16 = implicit
...

[casts]
i32 -> i8  = trunc
...

[common]
i8   , i16 = i16
...
```

`[types]` supports the categories `primitive`, `integer`, `float`, `opaque`,
`pointer`, `array`, `slice`, `function`, `optional`, `nominal`, and
`userdef`. `integer` and `float` require `bits`; `integer` can also carry
`signed` (default `false` for the current catalogue entries unless explicitly
marked).

`[coercions]` uses `implicit` or `explicit`. `[casts]` uses `trunc`,
`sign_extend`, `zero_extend`, `round`, `reinterpret`, `bitcast`, or
`ignored_maybe`. `[common]` declares `left , right = result`.

The generator rejects unknown sections, undeclared types, invalid `bits`,
`from == to` coercion/cast rows, duplicates, invalid cast/coercion kinds,
and missing required sections.

## Regeneration

```bash
python3 src/common/type-system/generate.py \
  src/common/type-system/type-system.rules \
  --out build/src/common/type-system
```

The build also regenerates the table automatically:

```bash
cmake --build build --target zct_type_system_generated -j
```

## Runtime API

```cpp
#include "common/type-system/type-system.hpp"

using toolkit::type_system::TypeContext;
using toolkit::type_system::TypeKind;

common::memory::Arena arena;
common::memory::StringInterner interner(arena);
TypeContext ctx(arena, interner);

const auto *i32 = ctx.find("i32");
const auto *i8 = ctx.find("i8");

const auto point =
    ctx.registerType<TypeKind::Struct>(interner.intern("Point"),
                                       std::span<const toolkit::type_system::TypeId>{
                                           i32->id, i32->id});

ctx.addCmp(point, point, comparisonResolver);
const auto *cmp = ctx.binaryResult("cmp", point, point);
```

Registered custom rules coexist with the generated static tables. Custom
coercion/cast/common rows win or complement generated rows without adding
duplicate keys. `addCmp` registers a binary `cmp` rule whose resolver can
compute both result type and future comparison semantics.

## Verification

```bash
cmake --build build --target zct_type_system_generated type-system-test -j
ctest --test-dir build -R 'type-system' --output-on-failure
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Current Limits

Unification, ownership, mutability, IR emission, and a fuller `Type` object
are intentionally out of scope. Unary/binary operator rows are runtime-only
and are not part of the rules file.
