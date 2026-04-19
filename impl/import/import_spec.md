# Import System Specification

## Overview

This document specifies the data structures for Zith's import system, featuring C ABI compatibility with C++ wrappers, visibility levels, and a global symbol table for resolution.

---

## 1. Design Principles

1. **C ABI First**: Base structures use only C-compatible types for FFI.
2. **Visibility Segmentation**: Symbols organized by `public`, `private`, `protected` (default).
3. **RAII in C++**: Wrapper provides automatic resource management.
4. **Dirty Tracking**: Flag indicates unsynchronized changes.
5. **Change Log**: Optional incremental change tracking.

---

## 2. Visibility Levels

| Level      | Description                      |
|-----------|----------------------------------|
| `public`  | Exported, visible to importers    |
| `protected` | Visible to derived/parents     |
| `private` | Internal, not exported           |

Corresponds to `ZithVisibility` enum in `types.hpp`.

---

## 3. Symbol Categories

For each visibility level, symbols are segmented into:

| Category      | Description                          |
|--------------|--------------------------------------|
| `types`      | `struct`, `union`, `component`, `entity`, `enum` |
| `functions`  | Functions/methods                    |
| `traits`     | Traits and families                   |

---

## 4. Data Structures

### 4.1 C ABI — `ZithImport`

```c
typedef struct ZithImport {
    // Metadata
    char*               name;           // Module/import name (owned)
    uint32_t            version;       // Version number

    // Visibility collections (arrays with length)
    // -- Public --
    ZithSymbolArray     public_types;
    ZithSymbolArray     public_functions;
    ZithSymbolArray     public_traits;

    // -- Protected --
    ZithSymbolArray     protected_types;
    ZithSymbolArray     protected_functions;
    ZithSymbolArray     protected_traits;

    // -- Private --
    ZithSymbolArray     private_types;
    ZithSymbolArray     private_functions;
    ZithSymbolArray     private_traits;

    // Change tracking
    uint8_t            is_dirty;       // 1 = modified since last sync
    ZithChangeArray     changes;        // Incremental changes (may be empty)
} ZithImport;
```

### 4.2 C ABI — `ZithSymbol`

```c
typedef struct ZithSymbol {
    char*               name;           // Symbol name
    ZithSymbolKind      kind;           // struct, union, function, trait, etc.
    ZithVisibility     visibility;     // public/private/protected
    void*              decl;           // Pointer to AST declaration
} ZithSymbol;
```

### 4.3 Symbol Kind Enum

```c
typedef enum ZithSymbolKind {
    ZITH_SYM_STRUCT    = 0,
    ZITH_SYM_UNION     = 1,
    ZITH_SYM_COMPONENT = 2,
    ZITH_SYM_ENTITY   = 3,
    ZITH_SYM_ENUM    = 4,
    ZITH_SYM_FUNCTION = 5,
    ZITH_SYM_TRAIT    = 6,
    ZITH_SYM_FAMILY   = 7,
} ZithSymbolKind;
```

### 4.4 Change Record

```c
typedef enum ZithChangeKind {
    ZITH_CHANGE_ADD    = 0,
    ZITH_CHANGE_REMOVE = 1,
    ZITH_CHANGE_MODIFY = 2,
} ZithChangeKind;

typedef struct ZithChange {
    ZithChangeKind    kind;
    char*            symbol_name;
    ZithSymbolKind   symbol_kind;
} ZithChange;
```

### 4.5 Array Helpers

```c
typedef struct ZithSymbolArray {
    ZithSymbol*   data;
    uint32_t      length;
    uint32_t      capacity;
} ZithSymbolArray;

typedef struct ZithChangeArray {
    ZithChange*   data;
    uint32_t      length;
    uint32_t      capacity;
} ZithChangeArray;
```

---

## 5. Symbol Table

### 5.1 Purpose

Centralized global resolution for all imported symbols:
- Fast lookup by name (O(1) average with `unordered_dense_map`)
- Collision handling for overloaded functions
- Supports versioning and aliasing

### 5.2 Structure

```c
typedef struct ZithSymbolTable {
    // Primary index: name -> Symbol*
    struct unordered_dense_map<char*, ZithSymbol*> primary;

    // Secondary index: visibility -> vector<Symbol*>
    struct unordered_dense_map<uint8_t, ZithSymbolArray> by_visibility;

    // Secondary index: kind -> vector<Symbol*>
    struct unordered_dense_map<uint8_t, ZithSymbolArray> by_kind;
} ZithSymbolTable;
```

---

## 6. C++ Wrapper

### 6.1 `ZithImport` Wrapper

```cpp
class Import {
public:
    Import(std::string name, uint32_t version = 0);
    ~Import();

    // Symbol addition (visibility inferred or explicit)
    void add_type(std::string name, SymbolKind kind, Visibility vis = Visibility::Public);
    void add_function(std::string name, Visibility vis = Visibility::Public);
    void add_trait(std::string name, Visibility vis = Visibility::Public);

    // Accessors by visibility
    auto& public_types() const;
    auto& protected_types() const;
    auto& private_types() const;

    // Dirty management
    bool is_dirty() const;
    void mark_clean();
    void mark_dirty();

    // Change log
    void log_change(ChangeKind kind, std::string symbol_name, SymbolKind sym_kind);
    span<const Change> changes() const;
    void clear_changes();

    // C ABI access
    ZithImport* to_c();
    static Import from_c(ZithImport* c_import);

private:
    ZithImport c_impl_;
};
```

### 6.2 RAII Principles

- **Constructor**: Initializes all arrays to empty, `is_dirty = 1`
- **Destructor**: Frees all owned memory (strings, arrays)
- **Move semantics**: Transfer ownership, source marked clean
- **Copy**: Deep copy on demand (clone method)

---

## 7. Global Symbol Table

### 7.1 Singleton Access

```cpp
class SymbolTable {
public:
    static SymbolTable& instance();

    void register_import(Import& imp);
    void unregister_import(const std::string& name);

    Symbol* resolve(const std::string& name);
    std::vector<Symbol*> resolve_all(const std::string& name);

    // Batch operations
    void register_bulk(Import& imp);

private:
    SymbolTable() = default;
    struct unordered_dense_map<std::string, Import*> imports_;
    ZithSymbolTable c_table_;
};
```

---

## 8. Usage Example

```cpp
// Create import
Import math("math", 1);

// Add symbols
math.add_type("Vec3", SymbolKind::Struct, Visibility::Public);
math.add_function("dot", Visibility::Public);
math.add_type("internal_cache", SymbolKind::Struct, Visibility::Private);

// Resolve globally
auto* sym = SymbolTable::instance().resolve("math.dot");
if (sym) {
    sym->decl; // void* to AST
}
```

---

## 9. Rationale

| Decision | Justification |
|----------|----------------|
| Separate visibility arrays | O(1) export filtering; ABI stability |
| `unordered_dense_map` | Faster than `std::unordered_map`, less memory |
| Dirty flag | Avoids full re-export on unchanged imports |
| Change log optional | Zero-cost when unused; enables incremental recompilation |
| C ABI first | Enables FFI with other languages |

---

## 10. File Layout

```
impl/import/
├── import_spec.md      # This document
├── import.h           # C ABI definitions
├── import.hpp         # C++ wrapper
└── symbol_table.hpp   # Global symbol resolution
```