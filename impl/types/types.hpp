// impl/types/types.hpp — Centralized type definitions for Zith
//
// Moves enums, node IDs, and core type aliases out of zith.hpp and ast.h
// to reduce dependency coupling and improve maintainability.
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Token Types — mirrored from ZithTokenType in zith.hpp
// The C API enum remains in zith.hpp; this header provides C++ helpers
// ============================================================================

#ifdef __cplusplus

// Forward declaration — actual enum lives in zith.hpp (C API contract)
// We re-export it here under a C++-friendly namespace for internal use.
#include <zith/zith.hpp>

namespace zith {
    using TokenType = ZithTokenType;
    using SourceLoc = ZithSourceLoc;
    using Token = ZithToken;
    using TokenStream = ZithTokenStream;
    using Str = ZithStr;
    using Slice = ZithSlice;
}

#endif // __cplusplus

// ============================================================================
// AST Node Types — extended node IDs (>= 1000)
// ============================================================================

enum ZithNodeExtendedId: uint16_t {
    // -- Expressions ---------------------------------------------------------
    ZITH_NODE_ARROW_CALL   = 1000,
    ZITH_NODE_CAST         = 1001,
    ZITH_NODE_OPTIONAL_EXPR = 1002,
    ZITH_NODE_UNWRAP       = 1003,
    ZITH_NODE_RANGE        = 1004,
    ZITH_NODE_LAMBDA       = 1005,
    ZITH_NODE_SPAWN_EXPR   = 1006,
    ZITH_NODE_RECURSE      = 1007,

    // -- Literals composite --------------------------------------------------
    ZITH_NODE_ARRAY_LIT    = 1020,
    ZITH_NODE_STRUCT_LIT   = 1021,
    ZITH_NODE_TUPLE_LIT    = 1022,

    // -- Declarations --------------------------------------------------------
    ZITH_NODE_PROGRAM      = 1030,
    ZITH_NODE_CONST_DECL   = 1031,
    ZITH_NODE_STRUCT_DECL  = 1032,
    ZITH_NODE_ENUM_DECL    = 1033,
    ZITH_NODE_TRAIT_DECL   = 1034,
    ZITH_NODE_IMPL_DECL    = 1035,
    ZITH_NODE_TYPE_ALIAS   = 1036,
    ZITH_NODE_COMPONENT_DECL = 1037,
    ZITH_NODE_UNION_DECL   = 1038,
    ZITH_NODE_FAMILY_DECL  = 1039,
    ZITH_NODE_ENTITY_DECL  = 1040,
    ZITH_NODE_MODULE_DECL  = 1041,
    ZITH_NODE_IMPORT       = 1042,
    ZITH_NODE_EXPORT       = 1043,

    // -- Statements ----------------------------------------------------------
    ZITH_NODE_SWITCH       = 1050,
    ZITH_NODE_CASE         = 1051,
    ZITH_NODE_BREAK        = 1052,
    ZITH_NODE_CONTINUE     = 1053,
    ZITH_NODE_GOTO         = 1054,
    ZITH_NODE_MARKER       = 1055,
    ZITH_NODE_ENTRY        = 1056,
    ZITH_NODE_SCENE        = 1057,
    ZITH_NODE_TRY_CATCH    = 1058,
    ZITH_NODE_SPAWN_STMT   = 1059,
    ZITH_NODE_AWAIT_STMT   = 1060,
    ZITH_NODE_YIELD        = 1061,
    ZITH_NODE_JOINED       = 1062,

    // -- Types ---------------------------------------------------------------
    ZITH_NODE_TYPE_OPTIONAL = 1070,
    ZITH_NODE_TYPE_RESULT   = 1071,
    ZITH_NODE_TYPE_ARRAY    = 1072,
    ZITH_NODE_TYPE_TUPLE    = 1073,
    ZITH_NODE_TYPE_POINTER  = 1074,
    ZITH_NODE_TYPE_UNIQUE   = 1075,
    ZITH_NODE_TYPE_SHARED   = 1076,
    ZITH_NODE_TYPE_VIEW     = 1077,
    ZITH_NODE_TYPE_LEND     = 1078,
    ZITH_NODE_TYPE_PACK     = 1079,

    // -- Auxiliary -----------------------------------------------------------
    ZITH_NODE_FIELD        = 1090,
    ZITH_NODE_ENUM_VARIANT = 1091,
    ZITH_NODE_MATCH_ARM    = 1092,
};

// ============================================================================
// Semantic enums
// ============================================================================

typedef enum ZithFnKind {
    ZITH_FN_NORMAL   = 0,
    ZITH_FN_ASYNC    = 1,
    ZITH_FN_NORETURN = 2,
    ZITH_FN_FLOWING  = 3,
} ZithFnKind;

typedef enum ZithBindingKind {
    ZITH_BINDING_LET        = 0,
    ZITH_BINDING_VAR        = 1,
    ZITH_BINDING_CONST      = 2,
    ZITH_BINDING_AUTO       = 3,
    ZITH_BINDING_GLOBAL     = 4,
    ZITH_BINDING_PERSISTENT = 5,
    ZITH_BINDING_LOCAL      = 6,
} ZithBindingKind;

typedef enum ZithOwnership {
    ZITH_OWN_DEFAULT  = 0,
    ZITH_OWN_VIEW     = 1,
    ZITH_OWN_LEND     = 2,
    ZITH_OWN_UNIQUE   = 3,
    ZITH_OWN_SHARED   = 4,
    ZITH_OWN_PACK     = 5,
} ZithOwnership;

typedef enum ZithVisibility {
    ZITH_VIS_PRIVATE    = 0,
    ZITH_VIS_PUBLIC     = 1,
    ZITH_VIS_PROTECTED  = 2,
} ZithVisibility;

typedef enum ZithLiteralKind {
    ZITH_LIT_INT     = 0,
    ZITH_LIT_UINT    = 1,
    ZITH_LIT_FLOAT   = 2,
    ZITH_LIT_STRING  = 3,
    ZITH_LIT_BOOL    = 4,
} ZithLiteralKind;

// ============================================================================
// Parser mode
// ============================================================================

typedef enum ZithParserMode {
    ZITH_MODE_SCAN  = 0,  // Signature-only, bodies captured as UNBODY
    ZITH_MODE_EXPAND = 1, // Parse UNBODY blocks into full BLOCK nodes
    ZITH_MODE_SEMA  = 2,  // Semantic analysis (name resolution, types, borrow check)
} ZithParserMode;

#ifdef __cplusplus
} // extern "C"
#endif
