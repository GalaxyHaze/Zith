# Common Runtime

## Purpose

`common` provides the small runtime pieces used by generated helpers and handwritten compiler
code. It intentionally replaces habitual `std::` containers for internal ownership, storage, and
result-shaped values when the local facility applies.

Prefer these types for new compiler-internal state:

| Type | Purpose |
|---|---|
| `memory::Arena` | Arena allocation and lifetime scope. |
| `memory::DynArray<T>` | Dynamic array backed by an arena. |
| `memory::FlatMap` | Small map implementation. |
| `memory::StringInterner` | Interned string storage and lookup. |
| `memory::SourceMap` | Source file identity and content lookup. |
| `memory::Optional<T>` | Optional value without `std::optional`. |
| `memory::Result<T, E>` | Value/error return. |

## Files

| File | Responsibility |
|---|---|
| `arena.hpp` / `arena.cpp` | Arena allocation and mark-point rollback support. |
| `dyn-array.hpp` | Arena-backed dynamic array. |
| `flat-map.hpp` | Small associative container. |
| `optional.hpp` | Optional wrapper. |
| `result.hpp` | Value/error result type. |
| `source-file.hpp` / `source-file.cpp` | Source file loading and mmap lifetime. |
| `source-map.hpp` / `source-map.cpp` | Source locations and file identity. |
| `span.hpp` | Offsets, spans, and file locations. |
| `string-interner.hpp` / `string-interner.cpp` | Interned string pool. |
| `CMakeLists.txt` | Library target `zith_common`. |

## Usage

### Arena Allocation

```cpp
memory::Arena arena;
auto *value = arena.make<MyType>(constructorArg);
auto *bytes = arena.alloc(bytesToAllocate, alignof(MyType));
```

The arena owns all allocations until it is reset or destroyed. Use `memory::MarkPoint` when a
rollback scope is needed.

### Dynamic Arrays

```cpp
memory::DynArray<int> values{arena};
values.push(1);
values.push(2);
```

`DynArray` must be constructed with an arena. It is non-copyable but movable.

### Strings

```cpp
memory::StringInterner interner{arena};
const auto id = interner.intern("source");
const auto view = interner.lookup(id);
```

The interner stores lifetime and lookup state in the arena.

### Results

```cpp
memory::Result<int> ok{42};
memory::Result<int> failed{memory::Error{"boom"}};

if (!ok)
    std::abort();
if (failed)
    std::abort();
```

`Result<void, E>` is supported. The value accessors abort when the result does not contain a value.

### Source Files And Maps

```cpp
memory::SourceMap sourceMap;
const auto id = sourceMap.loadFile("src/main.zith");
if (!id)
    return id.error();

const auto loc = sourceMap.get(id.value());
if (loc)
    return loc->get().slice();
```

`SourceMap` owns loaded source content and provides stable file ids for spans.

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
