// impl/import/import.h — C ABI for Zith's import system
//
// Provides C-compatible structures for the import system with visibility
// levels and symbol categorization. Use via import.hpp for C++.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Symbol Kinds
// ============================================================================

typedef enum ZithSymbolKind {
    ZITH_SYM_STRUCT    = 0,
    ZITH_SYM_UNION     = 1,
    ZITH_SYM_COMPONENT = 2,
    ZITH_SYM_ENTITY   = 3,
    ZITH_SYM_ENUM     = 4,
    ZITH_SYM_FUNCTION = 5,
    ZITH_SYM_TRAIT    = 6,
    ZITH_SYM_FAMILY   = 7,
} ZithSymbolKind;

// ============================================================================
// Visibility (mirrors ZithVisibility from types.hpp)
// ============================================================================

typedef enum ZithImportVisibility {
    ZITH_VIS_PRIVATE   = 0,
    ZITH_VIS_PUBLIC    = 1,
    ZITH_VIS_PROTECTED = 2,
} ZithImportVisibility;

// ============================================================================
// Arrays
// ============================================================================

typedef struct ZithSymbol {
    char*                 name;
    ZithSymbolKind         kind;
    ZithImportVisibility visibility;
    void*                decl;
} ZithSymbol;

typedef struct ZithChange {
    uint8_t           kind; // 0 = add, 1 = remove, 2 = modify
    char*             symbol_name;
    ZithSymbolKind    symbol_kind;
} ZithChange;

typedef struct ZithSymbolArray {
    ZithSymbol* data;
    uint32_t   length;
    uint32_t   capacity;
} ZithSymbolArray;

typedef struct ZithChangeArray {
    ZithChange* data;
    uint32_t    length;
    uint32_t    capacity;
} ZithChangeArray;

// ============================================================================
// Import (main structure)
// ============================================================================

typedef struct ZithImport {
    char*               name;
    uint32_t            version;

    // Public symbols
    ZithSymbolArray     public_types;
    ZithSymbolArray     public_functions;
    ZithSymbolArray     public_traits;

    // Protected symbols
    ZithSymbolArray     protected_types;
    ZithSymbolArray     protected_functions;
    ZithSymbolArray     protected_traits;

    // Private symbols
    ZithSymbolArray     private_types;
    ZithSymbolArray     private_functions;
    ZithSymbolArray     private_traits;

    // Change tracking
    uint8_t            is_dirty;
    ZithChangeArray     changes;
} ZithImport;

// ============================================================================
// Symbol Table
// ============================================================================

typedef struct ZithSymbolTable ZithSymbolTable;

// Creation/destruction
ZithImport*         zith_import_create(const char* name, uint32_t version);
void                zith_import_destroy(ZithImport* imp);

// Array operations
void                zith_symbol_array_init(ZithSymbolArray* arr, uint32_t capacity);
void                zith_symbol_array_free(ZithSymbolArray* arr);
void                zith_symbol_array_push(ZithSymbolArray* arr, ZithSymbol sym);

void                zith_change_array_init(ZithChangeArray* arr, uint32_t capacity);
void                zith_change_array_free(ZithChangeArray* arr);
void                zith_change_array_push(ZithChangeArray* arr, ZithChange change);

// Change tracking
void                zith_import_mark_dirty(ZithImport* imp);
void                zith_import_mark_clean(ZithImport* imp);
void                zith_import_clear_changes(ZithImport* imp);

// Symbol table operations
ZithSymbolTable*   zith_symbol_table_create(void);
void              zith_symbol_table_destroy(ZithSymbolTable* tbl);
void              zith_symbol_table_register(ZithSymbolTable* tbl, ZithImport* imp);
void              zith_symbol_table_unregister(ZithSymbolTable* tbl, const char* name);
ZithSymbol*        zith_symbol_table_resolve(ZithSymbolTable* tbl, const char* name);

#ifdef __cplusplus
}
#endif