// impl/types/types.hpp — Centralized type definitions for Kalidous
//
// Moves enums, node IDs, and core type aliases out of kalidous.hpp and ast.h
// to reduce dependency coupling and improve maintainability.
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Token Types — mirrored from KalidousTokenType in kalidous.hpp
// The C API enum remains in kalidous.hpp; this header provides C++ helpers
// ============================================================================

#ifdef __cplusplus

// Forward declaration — actual enum lives in kalidous.hpp (C API contract)
// We re-export it here under a C++-friendly namespace for internal use.
#include <kalidous/kalidous.hpp>

namespace kalidous {
    using TokenType = KalidousTokenType;
    using SourceLoc = KalidousSourceLoc;
    using Token = KalidousToken;
    using TokenStream = KalidousTokenStream;
    using Str = KalidousStr;
    using Slice = KalidousSlice;
}

#endif // __cplusplus

// ============================================================================
// AST Node Types — extended node IDs (>= 1000)
// ============================================================================

enum KalidousNodeExtendedId: uint16_t {
    // -- Expressions ---------------------------------------------------------
    KALIDOUS_NODE_ARROW_CALL   = 1000,
    KALIDOUS_NODE_CAST         = 1001,
    KALIDOUS_NODE_OPTIONAL_EXPR = 1002,
    KALIDOUS_NODE_UNWRAP       = 1003,
    KALIDOUS_NODE_RANGE        = 1004,
    KALIDOUS_NODE_LAMBDA       = 1005,
    KALIDOUS_NODE_SPAWN_EXPR   = 1006,
    KALIDOUS_NODE_RECURSE      = 1007,

    // -- Literals composite --------------------------------------------------
    KALIDOUS_NODE_ARRAY_LIT    = 1020,
    KALIDOUS_NODE_STRUCT_LIT   = 1021,
    KALIDOUS_NODE_TUPLE_LIT    = 1022,

    // -- Declarations --------------------------------------------------------
    KALIDOUS_NODE_PROGRAM      = 1030,
    KALIDOUS_NODE_CONST_DECL   = 1031,
    KALIDOUS_NODE_STRUCT_DECL  = 1032,
    KALIDOUS_NODE_ENUM_DECL    = 1033,
    KALIDOUS_NODE_TRAIT_DECL   = 1034,
    KALIDOUS_NODE_IMPL_DECL    = 1035,
    KALIDOUS_NODE_TYPE_ALIAS   = 1036,
    KALIDOUS_NODE_COMPONENT_DECL = 1037,
    KALIDOUS_NODE_UNION_DECL   = 1038,
    KALIDOUS_NODE_FAMILY_DECL  = 1039,
    KALIDOUS_NODE_ENTITY_DECL  = 1040,
    KALIDOUS_NODE_MODULE_DECL  = 1041,
    KALIDOUS_NODE_IMPORT       = 1042,
    KALIDOUS_NODE_EXPORT       = 1043,

    // -- Statements ----------------------------------------------------------
    KALIDOUS_NODE_SWITCH       = 1050,
    KALIDOUS_NODE_CASE         = 1051,
    KALIDOUS_NODE_BREAK        = 1052,
    KALIDOUS_NODE_CONTINUE     = 1053,
    KALIDOUS_NODE_GOTO         = 1054,
    KALIDOUS_NODE_MARKER       = 1055,
    KALIDOUS_NODE_ENTRY        = 1056,
    KALIDOUS_NODE_SCENE        = 1057,
    KALIDOUS_NODE_TRY_CATCH    = 1058,
    KALIDOUS_NODE_SPAWN_STMT   = 1059,
    KALIDOUS_NODE_AWAIT_STMT   = 1060,
    KALIDOUS_NODE_YIELD        = 1061,
    KALIDOUS_NODE_JOINED       = 1062,

    // -- Types ---------------------------------------------------------------
    KALIDOUS_NODE_TYPE_OPTIONAL = 1070,
    KALIDOUS_NODE_TYPE_RESULT   = 1071,
    KALIDOUS_NODE_TYPE_ARRAY    = 1072,
    KALIDOUS_NODE_TYPE_TUPLE    = 1073,
    KALIDOUS_NODE_TYPE_POINTER  = 1074,
    KALIDOUS_NODE_TYPE_UNIQUE   = 1075,
    KALIDOUS_NODE_TYPE_SHARED   = 1076,
    KALIDOUS_NODE_TYPE_VIEW     = 1077,
    KALIDOUS_NODE_TYPE_LEND     = 1078,
    KALIDOUS_NODE_TYPE_PACK     = 1079,

    // -- Auxiliary -----------------------------------------------------------
    KALIDOUS_NODE_FIELD        = 1090,
    KALIDOUS_NODE_ENUM_VARIANT = 1091,
    KALIDOUS_NODE_MATCH_ARM    = 1092,
};

// ============================================================================
// Semantic enums
// ============================================================================

typedef enum KalidousFnKind {
    KALIDOUS_FN_NORMAL   = 0,
    KALIDOUS_FN_ASYNC    = 1,
    KALIDOUS_FN_NORETURN = 2,
    KALIDOUS_FN_FLOWING  = 3,
} KalidousFnKind;

typedef enum KalidousBindingKind {
    KALIDOUS_BINDING_LET        = 0,
    KALIDOUS_BINDING_VAR        = 1,
    KALIDOUS_BINDING_CONST      = 2,
    KALIDOUS_BINDING_AUTO       = 3,
    KALIDOUS_BINDING_GLOBAL     = 4,
    KALIDOUS_BINDING_PERSISTENT = 5,
    KALIDOUS_BINDING_LOCAL      = 6,
} KalidousBindingKind;

typedef enum KalidousOwnership {
    KALIDOUS_OWN_DEFAULT  = 0,
    KALIDOUS_OWN_VIEW     = 1,
    KALIDOUS_OWN_LEND     = 2,
    KALIDOUS_OWN_UNIQUE   = 3,
    KALIDOUS_OWN_SHARED   = 4,
    KALIDOUS_OWN_PACK     = 5,
} KalidousOwnership;

typedef enum KalidousVisibility {
    KALIDOUS_VIS_PRIVATE    = 0,
    KALIDOUS_VIS_PUBLIC     = 1,
    KALIDOUS_VIS_PROTECTED  = 2,
} KalidousVisibility;

typedef enum KalidousLiteralKind {
    KALIDOUS_LIT_INT     = 0,
    KALIDOUS_LIT_UINT    = 1,
    KALIDOUS_LIT_FLOAT   = 2,
    KALIDOUS_LIT_STRING  = 3,
    KALIDOUS_LIT_BOOL    = 4,
} KalidousLiteralKind;

// ============================================================================
// Parser mode
// ============================================================================

typedef enum KalidousParserMode {
    KALIDOUS_MODE_SCAN  = 0,  // Signature-only, bodies captured as UNBODY
    KALIDOUS_MODE_EXPAND = 1, // Parse UNBODY blocks into full BLOCK nodes
    KALIDOUS_MODE_SEMA  = 2,  // Semantic analysis (name resolution, types, borrow check)
} KalidousParserMode;

#ifdef __cplusplus
} // extern "C"
#endif
