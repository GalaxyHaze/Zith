// kalidous_ast.h — Extended AST node IDs, payloads, constructors, and visitors
//
// All IDs below KALIDOUS_NODE_CUSTOM_START (1000) are defined in kalidous.h.
// Extended nodes live at 1000+ as required by the base header contract.
#pragma once

#include <kalidous/kalidous.hpp>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// How the KalidousNode union is used per node type
//
//  kids.a/b/c  — up to 3 child node pointers
//  list.ptr    — KalidousNode** or arena-allocated payload struct
//  list.len    — array length or payload discriminant
//  ident.str   — interned string (name, label)
//  ident.len   — string length
//  number.num  — double (float literals)
//  boolean     — bool (bool literals)
//  custom      — uint64_t (packed flags, enum values, token op codes)
//
// When a node needs more fields than the union provides (e.g. func_decl needs
// name + params[] + return_type + body + flags), list.ptr points to an
// arena-allocated payload struct defined below, and list.len holds the primary
// count (e.g. param_count).
// ============================================================================

// ============================================================================
// Extended node IDs (>= KALIDOUS_NODE_CUSTOM_START = 1000)
// ============================================================================

enum NodeTypes: unsigned short{
    // -- Expressions ---------------------------------------------------------
    KALIDOUS_NODE_ARROW_CALL = 1000, // a->b(...)   receiver + call node
    KALIDOUS_NODE_CAST = 1001, // expr as Type
    KALIDOUS_NODE_OPTIONAL_EXPR = 1002, // expr?
    KALIDOUS_NODE_UNWRAP = 1003, // expr!
    KALIDOUS_NODE_RANGE = 1004, // a..b  or  a...b
    KALIDOUS_NODE_LAMBDA = 1005, // |params| -> expr
    KALIDOUS_NODE_SPAWN_EXPR = 1006, // spawn expr
    KALIDOUS_NODE_RECURSE = 1007, // recurse(args) → CPS transform

    // -- Literals composite --------------------------------------------------
    KALIDOUS_NODE_ARRAY_LIT = 1020, // [a, b, c]
    KALIDOUS_NODE_STRUCT_LIT = 1021, // Point { x: 1, y: 2 }
    KALIDOUS_NODE_TUPLE_LIT = 1022, // (a, b, c)

    // -- Declarations --------------------------------------------------------
    KALIDOUS_NODE_PROGRAM = 1030, // root — list of top-level decls
    KALIDOUS_NODE_CONST_DECL = 1031, // const X = ...
    KALIDOUS_NODE_STRUCT_DECL = 1032, // struct Foo { ... }
    KALIDOUS_NODE_ENUM_DECL = 1033, // enum Color { ... }
    KALIDOUS_NODE_TRAIT_DECL = 1034, // trait Drawable { ... }
    KALIDOUS_NODE_IMPL_DECL = 1035, // implement Drawable for Foo { ... }
    KALIDOUS_NODE_TYPE_ALIAS = 1036, // using NewName = OldType
    KALIDOUS_NODE_COMPONENT_DECL = 1037,
    KALIDOUS_NODE_UNION_DECL = 1038,
    KALIDOUS_NODE_FAMILY_DECL = 1039,
    KALIDOUS_NODE_ENTITY_DECL = 1040,
    KALIDOUS_NODE_MODULE_DECL = 1041,
    KALIDOUS_NODE_IMPORT = 1042, // use foo::bar,

    // -- Statements ----------------------------------------------------------
    KALIDOUS_NODE_SWITCH = 1050, // switch expr { ... }
    KALIDOUS_NODE_CASE = 1051, // case pattern:
    KALIDOUS_NODE_BREAK = 1052, // break [label]
    KALIDOUS_NODE_CONTINUE = 1053, // continue [label]
    KALIDOUS_NODE_GOTO = 1054, // goto label  /  goto scene
    KALIDOUS_NODE_MARKER = 1055, // marker name(params) { body }
    KALIDOUS_NODE_ENTRY = 1056, // entry [name(params)] { body }
    KALIDOUS_NODE_SCENE = 1057, // scene { ... }
    KALIDOUS_NODE_TRY_CATCH = 1058, // try { } catch e { }
    KALIDOUS_NODE_SPAWN_STMT = 1059, // spawn { ... }
    KALIDOUS_NODE_AWAIT_STMT = 1060, // await expr
    KALIDOUS_NODE_YIELD = 1061, // yield expr  (async fn only)
    KALIDOUS_NODE_JOINED = 1062, // joined { ... }

    // -- Types ---------------------------------------------------------------
    KALIDOUS_NODE_TYPE_OPTIONAL = 1070, // Type?
    KALIDOUS_NODE_TYPE_RESULT = 1071, // Type!
    KALIDOUS_NODE_TYPE_ARRAY = 1072, // [N]Type  or  []Type
    KALIDOUS_NODE_TYPE_TUPLE = 1073, // (A, B, C)
    KALIDOUS_NODE_TYPE_POINTER = 1074, // *Type
    KALIDOUS_NODE_TYPE_UNIQUE = 1075, // unique Type
    KALIDOUS_NODE_TYPE_SHARED = 1076, // shared Type
    KALIDOUS_NODE_TYPE_VIEW = 1077, // view Type
    KALIDOUS_NODE_TYPE_LEND = 1078, // lend Type
    KALIDOUS_NODE_TYPE_PACK = 1079, // pack Type

    // -- Auxiliary -----------------------------------------------------------
    KALIDOUS_NODE_FIELD = 1090, // field of struct/component/entity
    KALIDOUS_NODE_ENUM_VARIANT = 1091, // enum/family variant
    KALIDOUS_NODE_MATCH_ARM = 1092, // switch/match arm
};

// ============================================================================
// Enums
// ============================================================================

typedef enum KalidousFnKind {
    KALIDOUS_FN_NORMAL = 0, // fn          — synchronous
    KALIDOUS_FN_ASYNC = 1, // async fn    — coroutine; may yield
    KALIDOUS_FN_NORETURN = 2, // noreturn fn — uses goto/marker; no return
    KALIDOUS_FN_FLOWING = 3, // flowing fn  — may use goto; has return
} KalidousFnKind;

typedef enum KalidousBindingKind {
    KALIDOUS_BINDING_LET = 0,
    KALIDOUS_BINDING_VAR = 1,
    KALIDOUS_BINDING_CONST = 2,
    KALIDOUS_BINDING_AUTO = 3,
    KALIDOUS_BINDING_GLOBAL = 4,
    KALIDOUS_BINDING_PERSISTENT = 5,
    KALIDOUS_BINDING_LOCAL = 6,
} KalidousBindingKind;

typedef enum KalidousOwnership {
    KALIDOUS_OWN_DEFAULT = 0,
    KALIDOUS_OWN_VIEW = 1,
    KALIDOUS_OWN_LEND = 2,
    KALIDOUS_OWN_UNIQUE = 3,
    KALIDOUS_OWN_SHARED = 4,
    KALIDOUS_OWN_PACK = 5,
} KalidousOwnership;

// Default is private — explicit declaration required to expose
typedef enum KalidousVisibility {
    KALIDOUS_VIS_PRIVATE = 0,
    KALIDOUS_VIS_PUBLIC = 1,
    KALIDOUS_VIS_PROTECTED = 2,
} KalidousVisibility;

// ============================================================================
// Arena-allocated payload structs
// Used when a node needs more fields than the union provides.
// Accessed via node->data.list.ptr (cast to the appropriate type).
// node->data.list.len holds the primary count for that payload.
// ============================================================================

// KALIDOUS_NODE_FUNC_DECL (201) — list.ptr, list.len = param_count
typedef struct {
    const char *name;
    size_t name_len;
    KalidousFnKind kind;
    KalidousNode **params;
    size_t param_count;
    KalidousNode *return_type; // NULL = void / inferred
    KalidousNode *body; // NULL = forward declaration
    KalidousVisibility visibility;
    bool is_extern;
} KalidousFuncPayload;

// KALIDOUS_NODE_VAR_DECL (200) — list.ptr, list.len = 0
typedef struct {
    const char *name;
    size_t name_len;
    KalidousBindingKind binding;
    KalidousOwnership ownership;
    KalidousVisibility visibility;
    KalidousNode *type_node; // NULL = inferred
    KalidousNode *initializer; // NULL = no initial value
} KalidousVarPayload;

// KALIDOUS_NODE_PARAM (202) — list.ptr, list.len = 0
typedef struct {
    const char *name;
    size_t name_len;
    KalidousOwnership ownership;
    KalidousNode *type_node;
    KalidousNode *default_value; // NULL = no default
    bool is_mutable;
} KalidousParamPayload;

// KALIDOUS_NODE_FIELD (1090) — list.ptr, list.len = 0
// Represents a named field in struct / component / entity
typedef struct {
    const char *name;
    size_t name_len;
    KalidousOwnership ownership;
    KalidousVisibility visibility;
    KalidousNode *type_node;
    KalidousNode *default_value; // NULL = no default
} KalidousFieldPayload;

// KALIDOUS_NODE_STRUCT_DECL (1032) — list.ptr, list.len = field_count
typedef struct {
    const char *name;
    size_t name_len;
    KalidousNode **fields;
    size_t field_count;
    KalidousNode **methods;
    size_t method_count;
    KalidousVisibility visibility;
} KalidousStructPayload;

// KALIDOUS_NODE_ENUM_DECL (1033) — list.ptr, list.len = variant_count
typedef struct {
    const char *name;
    size_t name_len;
    KalidousNode **variants;
    size_t variant_count;
    KalidousVisibility visibility;
} KalidousEnumPayload;

// KALIDOUS_NODE_UNION_DECL (1038) — list.ptr, list.len = type_count
typedef struct {
    const char *name;
    size_t name_len;
    KalidousNode **types;
    size_t type_count;
    KalidousVisibility visibility;
    bool is_raw;
} KalidousUnionPayload;

// KALIDOUS_NODE_SWITCH (1050) — list.ptr, list.len = arm_count
typedef struct {
    KalidousNode *subject;
    KalidousNode **arms;
    size_t arm_count;
    KalidousNode *default_arm;
} KalidousSwitchPayload;

// KALIDOUS_NODE_TRY_CATCH (1057) — list.ptr, list.len = 0
typedef struct {
    KalidousNode *try_block;
    const char *catch_var;
    size_t catch_var_len;
    KalidousNode *catch_block;
} KalidousTryCatchPayload;

// KALIDOUS_NODE_ENUM_VARIANT (1091)
// list → KalidousEnumVariantPayload (avoids union collision between ident and kids)
typedef struct {
    const char *name;
    size_t name_len;
    KalidousNode *value; // NULL = no explicit value
} KalidousEnumVariantPayload;

// KALIDOUS_NODE_GOTO (1054)
// list → KalidousGotoPayload
typedef struct {
    const char *target; // label name, or "scene" for goto scene
    size_t target_len;
    KalidousNode **args; // NULL if no arguments
    size_t arg_count;
    bool is_scene; // true = goto scene
} KalidousGotoPayload;

typedef struct {
    KalidousNode *callee;
    KalidousNode **args;
    size_t arg_count;
} KalidousCallPayload;

// KALIDOUS_NODE_FOR (302) — list.ptr, list.len = 0
typedef struct {
    KalidousNode *iterator_var; // for-in only
    KalidousNode *iterable; // for-in only
    KalidousNode *init; // classic for only
    KalidousNode *condition; // NULL = infinite
    KalidousNode *step; // classic for only
    KalidousNode *body;
    bool is_for_in;
} KalidousForPayload;

// KALIDOUS_NODE_IMPORT (1042) — list.ptr, list.len = path_len
typedef struct {
    const char *path;
    size_t path_len;
    KalidousVisibility vis;
} KalidousImportPayload;

// KALIDOUS_NODE_MARKER (1055) — named jump target with body
// KALIDOUS_NODE_ENTRY  (1056) — entry point (name is NULL for anonymous)
// list → KalidousMarkerPayload, list.len = param_count
// Only valid inside noreturn fn or flowing fn.
typedef struct {
    const char *name; // NULL = anonymous (entry only)
    size_t name_len;
    KalidousNode **params; // optional parameters
    size_t param_count;
    KalidousNode *body;
} KalidousMarkerPayload;

// ============================================================================
// Literal value — unified variant for all scalar literals
// Stored via list.ptr (arena-allocated KalidousLiteral payload)
// ============================================================================

typedef enum KalidousLiteralKind {
    KALIDOUS_LIT_INT, // signed decimal integer:  42, -7
    KALIDOUS_LIT_UINT, // unsigned (hex/bin/oct):  0xFF, 0b1010, 0o77
    KALIDOUS_LIT_FLOAT, // floating point:          3.14, 0.5
    KALIDOUS_LIT_STRING, // string literal:          "hello"
    KALIDOUS_LIT_BOOL, // boolean literal:         true, false
} KalidousLiteralKind;

typedef struct {
    KalidousLiteralKind kind;

    union {
        int64_t i64;
        uint64_t u64; // used for HEX, BINARY, OCTAL
        double f64;

        struct {
            const char *ptr;
            size_t len;
        } string;

        bool boolean;
    } value;
} KalidousLiteral;

// ============================================================================
// Union layout reference (inline comments in constructors)
//
//  NODE             | union slot        | notes
//  -----------------+-------------------+-------------------------------
//  LITERAL          | list → KalidousLiteral payload
//  IDENTIFIER       | ident.str/len     |
//  BINARY_OP        | kids.a/b          | custom = KalidousTokenType op
//  UNARY_OP         | kids.a            | custom = op | (is_postfix << 16)
//  CALL / RECURSE   | list → Payload    | list.len = arg_count
//  MEMBER (106)     | kids.a/b          | a=object, b=member ident
//  BLOCK            | list.ptr/len      | ptr=KalidousNode**, len=count
//  IF               | kids.a/b/c        | a=cond, b=then, c=else
//  RETURN / YIELD   | kids.a            | value (NULL = void)
//  AWAIT            | kids.a            | expr
//  VAR_DECL         | list → Payload    |
//  FUNC_DECL        | list → Payload    | list.len = param_count
//  PARAM            | list → Payload    |
//  STRUCT_DECL      | list → Payload    | list.len = field_count
//  ENUM_DECL        | list → Payload    | list.len = variant_count
//  SWITCH           | list → Payload    | list.len = arm_count
//  TRY_CATCH        | list → Payload    |
//  FOR              | list → Payload    |
//  PROGRAM          | list.ptr/len      | ptr=KalidousNode**, len=count
//  GOTO             | list → KalidousGotoPayload  | target name + optional args
//  MARKER / ENTRY   | list → KalidousMarkerPayload |
//  BREAK / CONTINUE | ident.str/len               | label (NULL = plain break)
//  ENUM_VARIANT     | list → KalidousEnumVariantPayload | name + optional value
//  SPAWN_STMT/EXPR  | kids.a            | body or expr
//  ARROW_CALL       | kids.a/b          | a=receiver, b=call node
//  CAST             | kids.a/b          | a=expr, b=type_node
//  IMPORT           | list → Payload    | list.len = path_len
//  ERROR            | ident.str/len     | message
// ============================================================================

// ============================================================================
// Constructors
// ============================================================================

KalidousNode *kalidous_ast_make_program(KalidousArena *a, KalidousNode **decls, size_t count);

KalidousNode * kalidous_ast_make_import_decl(KalidousArena * arena, KalidousSourceLoc loc, const KalidousImportPayload & decl);

KalidousNode *kalidous_ast_make_literal(KalidousArena *a, KalidousSourceLoc loc, const KalidousLiteral &lit);

KalidousNode *kalidous_ast_make_identifier(KalidousArena *a, KalidousSourceLoc loc, const char *name, size_t len);

KalidousNode *kalidous_ast_make_field(KalidousArena *a, KalidousSourceLoc loc, KalidousFieldPayload field);

KalidousNode *kalidous_ast_make_binary_op(KalidousArena *a, KalidousSourceLoc loc, KalidousTokenType op,
                                          KalidousNode *left, KalidousNode *right);

KalidousNode *kalidous_ast_make_unary_op(KalidousArena *a, KalidousSourceLoc loc, KalidousTokenType op,
                                         KalidousNode *operand, bool is_postfix);

KalidousNode *kalidous_ast_make_call(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *callee, KalidousNode **args,
                                     size_t arg_count);

KalidousNode *kalidous_ast_make_recurse(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *callee,
                                        KalidousNode **args, size_t arg_count);

KalidousNode *kalidous_ast_make_member(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *object,
                                       KalidousNode *member);

KalidousNode *kalidous_ast_make_arrow_call(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *receiver,
                                           KalidousNode *call);

KalidousNode *kalidous_ast_make_cast(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *expr,
                                     KalidousNode *type_node);

KalidousNode *kalidous_ast_make_var_decl(KalidousArena *a, KalidousSourceLoc loc, const KalidousVarPayload &decl);

KalidousNode *kalidous_ast_make_func_decl(KalidousArena *a, KalidousSourceLoc loc, const KalidousFuncPayload &decl);

KalidousNode *kalidous_ast_make_param(KalidousArena *a, KalidousSourceLoc loc, KalidousParamPayload param);

KalidousNode *kalidous_ast_make_block(KalidousArena *a, KalidousSourceLoc loc, KalidousNode **stmts, size_t count);

// Cria um nó UNBODY que armazena o token stream bruto entre { e }
// tokens aponta para o primeiro token após '{', token_count é o número de tokens até '}'
KalidousNode *kalidous_ast_make_unbody(KalidousArena *a, KalidousSourceLoc loc, 
                                        const KalidousToken *tokens, size_t token_count);

KalidousNode *kalidous_ast_make_if(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *cond, KalidousNode *then_br,
                                   KalidousNode *else_br);

KalidousNode *kalidous_ast_make_for(KalidousArena *a, KalidousSourceLoc loc, KalidousForPayload data);

KalidousNode *kalidous_ast_make_return(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *value);

KalidousNode *kalidous_ast_make_yield(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *value);

KalidousNode *kalidous_ast_make_await(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *expr);

KalidousNode *kalidous_ast_make_struct(KalidousArena *a, KalidousSourceLoc loc, KalidousStructPayload decl);

KalidousNode *kalidous_ast_make_enum(KalidousArena *a, KalidousSourceLoc loc, KalidousEnumPayload decl);

KalidousNode *kalidous_ast_make_union(KalidousArena *a, KalidousSourceLoc loc, const KalidousUnionPayload &decl);

KalidousNode *kalidous_ast_make_switch(KalidousArena *a, KalidousSourceLoc loc, KalidousSwitchPayload data);

KalidousNode *kalidous_ast_make_try_catch(KalidousArena *a, KalidousSourceLoc loc, KalidousTryCatchPayload data);

KalidousNode *kalidous_ast_make_import(KalidousArena *a, KalidousSourceLoc loc, KalidousImportPayload data);

KalidousNode *kalidous_ast_make_goto(KalidousArena *a, KalidousSourceLoc loc, KalidousGotoPayload data);

KalidousNode *kalidous_ast_make_marker(KalidousArena *a, KalidousSourceLoc loc, KalidousMarkerPayload data);

KalidousNode *kalidous_ast_make_entry(KalidousArena *a, KalidousSourceLoc loc, KalidousMarkerPayload data);

KalidousNode *kalidous_ast_make_enum_variant(KalidousArena *a, KalidousSourceLoc loc,
                                             const KalidousEnumVariantPayload &data);

KalidousNode *kalidous_ast_make_break(KalidousArena *a, KalidousSourceLoc loc, const char *label, size_t len);

KalidousNode *kalidous_ast_make_continue(KalidousArena *a, KalidousSourceLoc loc, const char *label, size_t len);

KalidousNode *kalidous_ast_make_spawn(KalidousArena *a, KalidousSourceLoc loc, KalidousNode *body, bool is_block);

KalidousNode *kalidous_ast_make_error(KalidousArena *a, KalidousSourceLoc loc, const char *msg);

// ============================================================================
// Visitor / Walker
// ============================================================================

typedef bool (*KalidousASTVisitorFn)(KalidousNode *node, void *userdata);

void kalidous_ast_walk(KalidousNode * root,
                       KalidousASTVisitorFn pre,
                       KalidousASTVisitorFn post,
                       void*userdata);

// ============================================================================
// Debug
// ============================================================================

void kalidous_ast_print(const KalidousNode *node, int indent);

const char *kalidous_ast_node_name(KalidousNodeId id);

const char *kalidous_ast_fn_kind_name(KalidousFnKind kind);

const char *kalidous_ast_literal_kind_name(KalidousLiteralKind kind);

const char *kalidous_ast_visibility_name(KalidousVisibility vis);

#ifdef __cplusplus
}
#endif