# Common Runtime

## Purpose

`common` provides the small runtime pieces used by generated helpers and handwritten compiler
code. It intentionally replaces habitual `std::` containers for internal ownership, storage, and
result-shaped values when the local facility applies.

Prefer these types for new compiler-internal state:

| Type | Purpose |
|---|---|
| `common::memory::Arena` | Arena allocation and lifetime scope. |
| `common::memory::DynArray<T>` | Dynamic array backed by an arena. |
| `common::memory::FlatMap` | Small map implementation. |
| `common::memory::StringInterner` | Interned string storage and lookup. |
| `common::memory::SourceMap` | Source file identity and content lookup. |
| `common::memory::Optional<T>` | Optional value without `std::optional`. |
| `common::memory::Result<T, E>` | Value/error return. |
| `common::diagnostic` | Diagnostic engine, renderer, Levenshtein distance, and suggestions. |
| `common::parser` | Output builder template used by generated parsers. |
| `common::text` | Parsing primitives emitted by generators. |

## Files

| Directory | Responsibility |
|---|---|
| `memory/` | Arena, arrays, maps, source files, spans, and result/optional types. |
| `ast/` | Template helpers for node-tree visits, clones, replacements, prunes, and transforms. |
| `diagnostic/` | Diagnostic type, renderer, and suggestion helpers. The declarative error catalogue lives in `src/diagnostic/`. |
| `parser/` | `OutputBuilder` used to assemble user-defined parse output. |
| `text/` | Parse helpers for booleans, integers, strings, and string lists. |
| `CMakeLists.txt` | Library target `zith_common`. |

## Usage

### Arena Allocation

```cpp
common::memory::Arena arena;
auto *value = arena.make<MyType>(constructorArg);
auto *bytes = arena.alloc(bytesToAllocate, alignof(MyType));
```

The arena owns all allocations until it is reset or destroyed. Use `common::memory::MarkPoint` when a
rollback scope is needed.

### Dynamic Arrays

```cpp
common::memory::DynArray<int> values{arena};
values.push(1);
values.push(2);
```

`DynArray` must be constructed with an arena. It is non-copyable but movable.

### Strings

```cpp
common::memory::StringInterner interner{arena};
const auto id = interner.intern("source");
const auto view = interner.lookup(id);
```

The interner stores lifetime and lookup state in the arena.

### Results

```cpp
common::memory::Result<int> ok{42};
common::memory::Result<int> failed{common::memory::Error{"boom"}};

if (!ok)
    std::abort();
if (failed)
    std::abort();
```

`Result<void, E>` is supported. The value accessors abort when the result does not contain a value.

### Source Files And Maps

```cpp
common::memory::SourceMap sourceMap;
const auto id = sourceMap.loadFile("src/main.zith");
if (!id)
    return id.error();

const auto loc = sourceMap.get(id.value());
if (loc)
    return loc->get().slice();
```

`SourceMap` owns loaded source content and provides stable file ids for spans.

### AST Helpers

An AST type can use `common/ast/` helpers when its namespace declares
`for_each_child(T *, Fn &&)`. The helper walks children with that function and never
depends on a generated AST shape. `for_each_child` must invoke `fn` with each
child pointer by reference so mutation helpers can replace or prune a child.

```cpp
template <typename Fn>
void for_each_child(BinExpr *node, Fn &&fn) {
    fn(node->left);   // passes Expr *& so helpers can update the field
    fn(node->right);
}

common::ast::visit(root, [](auto *node, auto *parent) {
    // Pre-order, parent is nullptr for the root.
});

AstRoot target{arena};
auto *copyRoot = common::ast::cloneTree(target, sourceRoot);
```

`replaceChild` and `pruneChild` return whether a direct child was updated. `transform`
does a post-order walk and replaces the returned node when `fn` returns non-null.

## Linking

Link `zith_common` into a target:

```cmake
target_link_libraries(my_target PRIVATE zith_common)
```

The target propagates public include directories needed by the runtime headers.

## Verification

```bash
cmake --build build -j
ctest --test-dir build -R source-map --output-on-failure
```

## Runtime Boundaries

`src/common/` is handwritten runtime support, not generated surface. Keep it focused on stable
types and behavior shared by generators or compiler code. Do not add ad-hoc parser/generator
logic here; shared Python generator logic belongs in `tools/rules_kit/`.
