// zith_ast.h — Extended AST node IDs, payloads, constructors, and visitors
//
// Refactored to use centralized modules:
//   - types/types.hpp for enums (ZithFnKind, ZithBindingKind, etc.)
//   - memory/arena.hpp for allocation
//
// All IDs below ZITH_NODE_CUSTOM_START (1000) are defined in zith.h.
// Extended nodes live at 1000+ as required by the base header contract.
#pragma once

#include <zith/zith.hpp>
#include "../types/types.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// How the ZithNode union is used per node type
//
//  kids.a/b/c  — up to 3 child node pointers
//  list.ptr    — ZithNode** or arena-allocated payload struct
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

// Extended node IDs are defined in types/types.hpp as ZithNodeExtendedId
// Use those values directly — no local redefinition needed.

// ============================================================================
// Arena-allocated payload structs
// Used when a node needs more fields than the union provides.
// Accessed via node->data.list.ptr (cast to the appropriate type).
// node->data.list.len holds the primary count for that payload.
// ============================================================================

// ZITH_NODE_FUNC_DECL (201) — list.ptr, list.len = param_count
typedef struct {
    const char *name;
    size_t name_len;
    ZithFnKind kind;
    ZithNode **params;
    size_t param_count;
    ZithNode *return_type; // NULL = void / inferred
    ZithNode *body; // NULL = forward declaration
    ZithVisibility visibility;
    bool is_extern;
} ZithFuncPayload;

// ZITH_NODE_VAR_DECL (200) — list.ptr, list.len = 0
typedef struct {
    const char *name;
    size_t name_len;
    ZithBindingKind binding;
    ZithOwnership ownership;
    ZithVisibility visibility;
    ZithNode *type_node; // NULL = inferred
    ZithNode *initializer; // NULL = no initial value
} ZithVarPayload;

// ZITH_NODE_PARAM (202) — list.ptr, list.len = 0
typedef struct {
    const char *name;
    size_t name_len;
    ZithOwnership ownership;
    ZithNode *type_node;
    ZithNode *default_value; // NULL = no default
    bool is_mutable;
} ZithParamPayload;

// ZITH_NODE_FIELD (1090) — list.ptr, list.len = 0
// Represents a named field in struct / component / entity
typedef struct {
    const char *name;
    size_t name_len;
    ZithOwnership ownership;
    ZithVisibility visibility;
    ZithNode *type_node;
    ZithNode *default_value; // NULL = no default
} ZithFieldPayload;

// ZITH_NODE_STRUCT_DECL (1032) — list.ptr, list.len = field_count
typedef struct {
    const char *name;
    size_t name_len;
    ZithNode **fields;
    size_t field_count;
    ZithNode **methods;
    size_t method_count;
    ZithVisibility visibility;
} ZithStructPayload;

// ZITH_NODE_ENUM_DECL (1033) — list.ptr, list.len = variant_count
typedef struct {
    const char *name;
    size_t name_len;
    ZithNode **variants;
    size_t variant_count;
    ZithVisibility visibility;
} ZithEnumPayload;

// ZITH_NODE_UNION_DECL (1038) — list.ptr, list.len = type_count
typedef struct {
    const char *name;
    size_t name_len;
    ZithNode **types;
    size_t type_count;
    ZithVisibility visibility;
    bool is_raw;
} ZithUnionPayload;

// ZITH_NODE_SWITCH (1050) — list.ptr, list.len = arm_count
typedef struct {
    ZithNode *subject;
    ZithNode **arms;
    size_t arm_count;
    ZithNode *default_arm;
} ZithSwitchPayload;

// ZITH_NODE_TRY_CATCH (1057) — list.ptr, list.len = 0
typedef struct {
    ZithNode *try_block;
    const char *catch_var;
    size_t catch_var_len;
    ZithNode *catch_block;
} ZithTryCatchPayload;

// ZITH_NODE_ENUM_VARIANT (1091)
// list → ZithEnumVariantPayload (avoids union collision between ident and kids)
typedef struct {
    const char *name;
    size_t name_len;
    ZithNode *value; // NULL = no explicit value
} ZithEnumVariantPayload;

// ZITH_NODE_GOTO (1054)
// list → ZithGotoPayload
typedef struct {
    const char *target; // label name, or "scene" for goto scene
    size_t target_len;
    ZithNode **args; // NULL if no arguments
    size_t arg_count;
    bool is_scene; // true = goto scene
} ZithGotoPayload;

typedef struct {
    ZithNode *callee;
    ZithNode **args;
    size_t arg_count;
} ZithCallPayload;

// ZITH_NODE_FOR (302) — list.ptr, list.len = 0
typedef struct {
    ZithNode *iterator_var; // for-in only
    ZithNode *iterable; // for-in only
    ZithNode *init; // classic for only
    ZithNode *condition; // NULL = infinite
    ZithNode *step; // classic for only
    ZithNode *body;
    bool is_for_in;
} ZithForPayload;

// ZITH_NODE_IMPORT (1042) — list.ptr, list.len = path_len
// ZITH_NODE_EXPORT (1043) — list.ptr, list.len = path_len
// Para sintaxe 'from': path = módulo base, alias = itens importados
typedef struct {
    const char *path;
    size_t path_len;
    ZithVisibility vis;
    const char *alias;      // para "as alias" (NULL se não tiver)
    size_t alias_len;
    bool is_export;         // true = export, false = import
    bool is_from;           // true = sintaxe "from x import y"
} ZithImportPayload;

// ZITH_NODE_MARKER (1055) — named jump target with body
// ZITH_NODE_ENTRY  (1056) — entry point (name is NULL for anonymous)
// list → ZithMarkerPayload, list.len = param_count
// Only valid inside noreturn fn or flowing fn.
typedef struct {
    const char *name; // NULL = anonymous (entry only)
    size_t name_len;
    ZithNode **params; // optional parameters
    size_t param_count;
    ZithNode *body;
} ZithMarkerPayload;

// ============================================================================
// Literal value — unified variant for all scalar literals
// ZithLiteralKind is defined in types/types.hpp — included above
// Stored via list.ptr (arena-allocated ZithLiteral payload)
// ============================================================================

typedef struct {
    ZithLiteralKind kind;

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
} ZithLiteral;

// ============================================================================
// Union layout reference (inline comments in constructors)
//
//  NODE             | union slot        | notes
//  -----------------+-------------------+-------------------------------
//  LITERAL          | list → ZithLiteral payload
//  IDENTIFIER       | ident.str/len     |
//  BINARY_OP        | kids.a/b          | custom = ZithTokenType op
//  UNARY_OP         | kids.a            | custom = op | (is_postfix << 16)
//  CALL / RECURSE   | list → Payload    | list.len = arg_count
//  MEMBER (106)     | kids.a/b          | a=object, b=member ident
//  BLOCK            | list.ptr/len      | ptr=ZithNode**, len=count
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
//  PROGRAM          | list.ptr/len      | ptr=ZithNode**, len=count
//  GOTO             | list → ZithGotoPayload  | target name + optional args
//  MARKER / ENTRY   | list → ZithMarkerPayload |
//  BREAK / CONTINUE | ident.str/len               | label (NULL = plain break)
//  ENUM_VARIANT     | list → ZithEnumVariantPayload | name + optional value
//  SPAWN_STMT/EXPR  | kids.a            | body or expr
//  ARROW_CALL       | kids.a/b          | a=receiver, b=call node
//  CAST             | kids.a/b          | a=expr, b=type_node
//  IMPORT           | list → Payload    | list.len = path_len
//  ERROR            | ident.str/len     | message
// ============================================================================

// ============================================================================
// Constructors
// ============================================================================

ZithNode *zith_ast_make_program(ZithArena *a, ZithNode **decls, size_t count);

ZithNode * zith_ast_make_import_decl(ZithArena * arena, ZithSourceLoc loc, const ZithImportPayload & decl);

ZithNode *zith_ast_make_literal(ZithArena *a, ZithSourceLoc loc, const ZithLiteral &lit);

ZithNode *zith_ast_make_identifier(ZithArena *a, ZithSourceLoc loc, const char *name, size_t len);

ZithNode *zith_ast_make_field(ZithArena *a, ZithSourceLoc loc, ZithFieldPayload field);

ZithNode *zith_ast_make_binary_op(ZithArena *a, ZithSourceLoc loc, ZithTokenType op,
                                          ZithNode *left, ZithNode *right);

ZithNode *zith_ast_make_unary_op(ZithArena *a, ZithSourceLoc loc, ZithTokenType op,
                                         ZithNode *operand, bool is_postfix);

ZithNode *zith_ast_make_call(ZithArena *a, ZithSourceLoc loc, ZithNode *callee, ZithNode **args,
                                     size_t arg_count);

ZithNode *zith_ast_make_recurse(ZithArena *a, ZithSourceLoc loc, ZithNode *callee,
                                        ZithNode **args, size_t arg_count);

ZithNode *zith_ast_make_member(ZithArena *a, ZithSourceLoc loc, ZithNode *object,
                                       ZithNode *member);

ZithNode *zith_ast_make_arrow_call(ZithArena *a, ZithSourceLoc loc, ZithNode *receiver,
                                           ZithNode *call);

ZithNode *zith_ast_make_cast(ZithArena *a, ZithSourceLoc loc, ZithNode *expr,
                                     ZithNode *type_node);

ZithNode *zith_ast_make_var_decl(ZithArena *a, ZithSourceLoc loc, const ZithVarPayload &decl);

ZithNode *zith_ast_make_func_decl(ZithArena *a, ZithSourceLoc loc, const ZithFuncPayload &decl);

ZithNode *zith_ast_make_param(ZithArena *a, ZithSourceLoc loc, ZithParamPayload param);

ZithNode *zith_ast_make_block(ZithArena *a, ZithSourceLoc loc, ZithNode **stmts, size_t count);

// Cria um nó UNBODY que armazena o token stream bruto entre { e }
// tokens aponta para o primeiro token após '{', token_count é o número de tokens até '}'
ZithNode *zith_ast_make_unbody(ZithArena *a, ZithSourceLoc loc, 
                                        const ZithToken *tokens, size_t token_count);

ZithNode *zith_ast_make_if(ZithArena *a, ZithSourceLoc loc, ZithNode *cond, ZithNode *then_br,
                                   ZithNode *else_br);

ZithNode *zith_ast_make_for(ZithArena *a, ZithSourceLoc loc, ZithForPayload data);

ZithNode *zith_ast_make_return(ZithArena *a, ZithSourceLoc loc, ZithNode *value);

ZithNode *zith_ast_make_yield(ZithArena *a, ZithSourceLoc loc, ZithNode *value);

ZithNode *zith_ast_make_await(ZithArena *a, ZithSourceLoc loc, ZithNode *expr);

ZithNode *zith_ast_make_struct(ZithArena *a, ZithSourceLoc loc, ZithStructPayload decl);

ZithNode *zith_ast_make_enum(ZithArena *a, ZithSourceLoc loc, ZithEnumPayload decl);

ZithNode *zith_ast_make_union(ZithArena *a, ZithSourceLoc loc, const ZithUnionPayload &decl);

ZithNode *zith_ast_make_switch(ZithArena *a, ZithSourceLoc loc, ZithSwitchPayload data);

ZithNode *zith_ast_make_try_catch(ZithArena *a, ZithSourceLoc loc, ZithTryCatchPayload data);

ZithNode *zith_ast_make_import(ZithArena *a, ZithSourceLoc loc, ZithImportPayload data);

ZithNode *zith_ast_make_goto(ZithArena *a, ZithSourceLoc loc, ZithGotoPayload data);

ZithNode *zith_ast_make_marker(ZithArena *a, ZithSourceLoc loc, ZithMarkerPayload data);

ZithNode *zith_ast_make_entry(ZithArena *a, ZithSourceLoc loc, ZithMarkerPayload data);

ZithNode *zith_ast_make_enum_variant(ZithArena *a, ZithSourceLoc loc,
                                             const ZithEnumVariantPayload &data);

ZithNode *zith_ast_make_break(ZithArena *a, ZithSourceLoc loc, const char *label, size_t len);

ZithNode *zith_ast_make_continue(ZithArena *a, ZithSourceLoc loc, const char *label, size_t len);

ZithNode *zith_ast_make_spawn(ZithArena *a, ZithSourceLoc loc, ZithNode *body, bool is_block);

ZithNode *zith_ast_make_error(ZithArena *a, ZithSourceLoc loc, const char *msg);

// ============================================================================
// Visitor / Walker
// ============================================================================

typedef bool (*ZithASTVisitorFn)(ZithNode *node, void *userdata);

void zith_ast_walk(ZithNode * root,
                       ZithASTVisitorFn pre,
                       ZithASTVisitorFn post,
                       void*userdata);

// ============================================================================
// Debug
// ============================================================================

void zith_ast_print(const ZithNode *node, int indent);

const char *zith_ast_node_name(ZithNodeId id);

const char *zith_ast_fn_kind_name(ZithFnKind kind);

const char *zith_ast_literal_kind_name(ZithLiteralKind kind);

const char *zith_ast_visibility_name(ZithVisibility vis);

#ifdef __cplusplus
} // extern "C"

// C++ alias for NodeTypes enum
using NodeTypes = ZithNodeExtendedId;
#endif