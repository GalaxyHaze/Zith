// impl/ast/ast.cpp — AST constructors, walker, and debug
//
// Refactored to use centralized modules:
//   - types/types.hpp for enums
//   - memory/arena.hpp for allocation
//   - diagnostics/diagnostics.hpp for debug output
#include "../memory/arena.hpp"
#include "ast.h"
#include "../lexer/debug.h"
#include "../types/types.hpp"
#include "../diagnostics/diagnostics.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>

// ============================================================================
// Internal helpers — C++ linkage only, not exported
// ============================================================================

static ZithNode *alloc_node(ZithArena *a, ZithNodeId id,
                                ZithSourceLoc loc) {
    auto *n = static_cast<ZithNode *>(zith_arena_alloc(a, sizeof(ZithNode)));
    if (!n) return nullptr;
    std::memset(n, 0, sizeof(ZithNode));
    n->type = id;
    n->loc = loc;
    return n;
}

// Template must be outside extern "C" — C linkage and templates are incompatible
template<typename T>
static T *alloc_payload(ZithArena *a, ZithNode *n) {
    auto *p = static_cast<T *>(zith_arena_alloc(a, sizeof(T)));
    if (!p) return nullptr;
    std::memset(p, 0, sizeof(T));
    n->data.list.ptr = p;
    return p;
}

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constructors
// ============================================================================

ZithNode *zith_ast_make_program(ZithArena *a,
                                        ZithNode **decls, size_t count) {
    ZithSourceLoc root = {0, 1};
    ZithNode *n = alloc_node(a, ZITH_NODE_PROGRAM, root);
    if (!n) return nullptr;
    n->data.list.ptr = decls;
    n->data.list.len = count;
    return n;
}

// list → ZithLiteral payload
// Strings are interned via zith_arena_str (lexemes are not null-terminated)
ZithNode *zith_ast_make_literal(ZithArena *a, ZithSourceLoc loc,
                                        const ZithLiteral &lit) {
    ZithNode *n = alloc_node(a, ZITH_NODE_LITERAL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithLiteral>(a, n);
    if (!p) return n;
    *p = lit;
    // Intern the string so the node owns its bytes independent of token lifetime
    if (lit.kind == ZITH_LIT_STRING && lit.value.string.ptr)
        p->value.string.ptr = zith_arena_str(a, lit.value.string.ptr,
                                                 lit.value.string.len);
    return n;
}

ZithNode *zith_ast_make_identifier(ZithArena *a, ZithSourceLoc loc,
                                           const char *name, size_t len) {
    ZithNode *n = alloc_node(a, ZITH_NODE_IDENTIFIER, loc);
    if (!n) return nullptr;
    n->data.ident.str = zith_arena_strdup(a, name);
    n->data.ident.len = len;
    return n;
}

// Union layout for BINARY_OP:
//   kids.a   (offset  0) = left  operand
//   list.len (offset  8) = operator token type  ← cannot use kids.b here (same offset!)
//   kids.c   (offset 16) = right operand
// kids.b and list.len are at the same offset in the union — using kids.b for right
// and then writing list.len = op would overwrite the right pointer.
ZithNode *zith_ast_make_binary_op(ZithArena *a, ZithSourceLoc loc,
                                          ZithTokenType op,
                                          ZithNode *left, ZithNode *right) {
    ZithNode *n = alloc_node(a, ZITH_NODE_BINARY_OP, loc);
    if (!n) return nullptr;
    n->data.kids.a = left;
    n->data.list.len = static_cast<size_t>(op);
    n->data.kids.c = right; // offset 16 — safe, does not alias list.len
    return n;
}

// Union layout for UNARY_OP:
//   kids.a   (offset  0) = operand
//   list.len (offset  8) = op | (is_postfix << 16)
ZithNode *zith_ast_make_unary_op(ZithArena *a, ZithSourceLoc loc,
                                         ZithTokenType op,
                                         ZithNode *operand, bool is_postfix) {
    ZithNode *n = alloc_node(a, ZITH_NODE_UNARY_OP, loc);
    if (!n) return nullptr;
    n->data.kids.a = operand;
    n->data.list.len = static_cast<size_t>(op) | (static_cast<size_t>(is_postfix) << 16);
    return n;
}

// list → ZithCallPayload, list.len = arg_count
ZithNode *zith_ast_make_call(ZithArena *a, ZithSourceLoc loc,
                                     ZithNode *callee,
                                     ZithNode **args, size_t arg_count) {
    ZithNode *n = alloc_node(a, ZITH_NODE_CALL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithCallPayload>(a, n);
    if (!p) return n;
    p->callee = callee;
    p->args = args;
    p->arg_count = arg_count;
    n->data.list.len = arg_count;
    return n;
}

// Same payload as call, different node id — sema validates context
ZithNode *zith_ast_make_recurse(ZithArena *a, ZithSourceLoc loc,
                                        ZithNode *callee,
                                        ZithNode **args, size_t arg_count) {
    ZithNode *n = alloc_node(a, ZITH_NODE_RECURSE, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithCallPayload>(a, n);
    if (!p) return n;
    p->callee = callee;
    p->args = args;
    p->arg_count = arg_count;
    n->data.list.len = arg_count;
    return n;
}

// kids.a = object, kids.b = member identifier node
ZithNode *zith_ast_make_member(ZithArena *a, ZithSourceLoc loc,
                                       ZithNode *object, ZithNode *member) {
    ZithNode *n = alloc_node(a, ZITH_NODE_MEMBER, loc);
    if (!n) return nullptr;
    n->data.kids.a = object;
    n->data.kids.b = member;
    return n;
}

// kids.a = receiver, kids.b = call node (ZITH_NODE_CALL)
ZithNode *zith_ast_make_arrow_call(ZithArena *a, ZithSourceLoc loc,
                                           ZithNode *receiver, ZithNode *call) {
    ZithNode *n = alloc_node(a, ZITH_NODE_ARROW_CALL, loc);
    if (!n) return nullptr;
    n->data.kids.a = receiver;
    n->data.kids.b = call;
    return n;
}

// kids.a = expr, kids.b = type_node
ZithNode *zith_ast_make_cast(ZithArena *a, const ZithSourceLoc loc,
                                     ZithNode *expr, ZithNode *type_node) {
    ZithNode *n = alloc_node(a, ZITH_NODE_CAST, loc);
    if (!n) return nullptr;
    n->data.kids.a = expr;
    n->data.kids.b = type_node;
    return n;
}

// list → ZithVarPayload
ZithNode *zith_ast_make_var_decl(ZithArena *a, const ZithSourceLoc loc,
                                         const ZithVarPayload &decl) {
    ZithNode *n = alloc_node(a, ZITH_NODE_VAR_DECL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithVarPayload>(a, n);
    if (!p) return n;
    *p = decl;
    p->name = zith_arena_strdup(a, decl.name);
    return n;
}

// list → ZithFuncPayload, list.len = param_count
ZithNode *zith_ast_make_func_decl(ZithArena *a, const ZithSourceLoc loc,
                                          const ZithFuncPayload &decl) {
    ZithNode *n = alloc_node(a, ZITH_NODE_FUNC_DECL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithFuncPayload>(a, n);
    if (!p) return n;
    *p = decl;
    p->name = zith_arena_strdup(a, decl.name);
    n->data.list.len = decl.param_count;
    return n;
}

// list → ZithParamPayload
ZithNode *zith_ast_make_param(ZithArena *a, ZithSourceLoc loc,
                                      ZithParamPayload param) {
    ZithNode *n = alloc_node(a, ZITH_NODE_PARAM, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithParamPayload>(a, n);
    if (!p) return n;
    *p = param;
    p->name = zith_arena_strdup(a, param.name);
    return n;
}

// list.ptr = ZithNode**, list.len = count
ZithNode *zith_ast_make_block(ZithArena *a, ZithSourceLoc loc,
                                      ZithNode **stmts, size_t count) {
    ZithNode *n = alloc_node(a, ZITH_NODE_BLOCK, loc);
    if (!n) return nullptr;
    n->data.list.ptr = stmts;
    n->data.list.len = count;
    return n;
}

// UNBODY: armazena token stream bruto entre { e } para parseamento posterior
// list.ptr = ZithToken*, list.len = token_count
ZithNode *zith_ast_make_unbody(ZithArena *a, ZithSourceLoc loc,
                                        const ZithToken *tokens, size_t token_count) {
    ZithNode *n = alloc_node(a, ZITH_NODE_UNBODY, loc);
    if (!n) return nullptr;
    n->data.list.ptr = const_cast<ZithToken*>(tokens);
    n->data.list.len = token_count;
    return n;
}

// kids.a = condition, kids.b = then_branch, kids.c = else_branch (NULL ok)
ZithNode *zith_ast_make_if(ZithArena *a, ZithSourceLoc loc,
                                   ZithNode *cond,
                                   ZithNode *then_br, ZithNode *else_br) {
    ZithNode *n = alloc_node(a, ZITH_NODE_IF, loc);
    if (!n) return nullptr;
    n->data.kids.a = cond;
    n->data.kids.b = then_br;
    n->data.kids.c = else_br;
    return n;
}

// list → ZithForPayload
ZithNode *zith_ast_make_for(ZithArena *a, ZithSourceLoc loc,
                                    ZithForPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_FOR, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithForPayload>(a, n);
    if (!p) return n;
    *p = data;
    return n;
}

// kids.a = value (NULL = void return)
ZithNode *zith_ast_make_return(ZithArena *a, ZithSourceLoc loc,
                                       ZithNode *value) {
    ZithNode *n = alloc_node(a, ZITH_NODE_RETURN, loc);
    if (!n) return nullptr;
    n->data.kids.a = value;
    return n;
}

// kids.a = value (NULL = bare yield)
ZithNode *zith_ast_make_yield(ZithArena *a, ZithSourceLoc loc,
                                      ZithNode *value) {
    ZithNode *n = alloc_node(a, ZITH_NODE_YIELD, loc);
    if (!n) return nullptr;
    n->data.kids.a = value;
    return n;
}

// kids.a = expr
ZithNode *zith_ast_make_await(ZithArena *a, ZithSourceLoc loc,
                                      ZithNode *expr) {
    ZithNode *n = alloc_node(a, ZITH_NODE_AWAIT_STMT, loc);
    if (!n) return nullptr;
    n->data.kids.a = expr;
    return n;
}

// list → ZithStructPayload, list.len = field_count
ZithNode *zith_ast_make_struct(ZithArena *a, ZithSourceLoc loc,
                                       ZithStructPayload decl) {
    ZithNode *n = alloc_node(a, ZITH_NODE_STRUCT_DECL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithStructPayload>(a, n);
    if (!p) return n;
    *p = decl;
    p->name = zith_arena_strdup(a, decl.name);
    n->data.list.len = decl.field_count;
    return n;
}

// list → ZithEnumPayload, list.len = variant_count
ZithNode *zith_ast_make_enum(ZithArena *a, ZithSourceLoc loc,
                                     ZithEnumPayload decl) {
    ZithNode *n = alloc_node(a, ZITH_NODE_ENUM_DECL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithEnumPayload>(a, n);
    if (!p) return n;
    *p = decl;
    p->name = zith_arena_strdup(a, decl.name);
    n->data.list.len = decl.variant_count;
    return n;
}

// list → ZithSwitchPayload, list.len = arm_count
ZithNode *zith_ast_make_switch(ZithArena *a, ZithSourceLoc loc,
                                       ZithSwitchPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_SWITCH, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithSwitchPayload>(a, n);
    if (!p) return n;
    *p = data;
    n->data.list.len = data.arm_count;
    return n;
}

// list → ZithTryCatchPayload
ZithNode *zith_ast_make_try_catch(ZithArena *a, ZithSourceLoc loc,
                                          ZithTryCatchPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_TRY_CATCH, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithTryCatchPayload>(a, n);
    if (!p) return n;
    *p = data;
    if (data.catch_var && data.catch_var_len)
        p->catch_var = zith_arena_strdup(a, data.catch_var);
    return n;
}

// list → ZithImportPayload, list.len = path_len
// Usado para IMPORT e EXPORT
ZithNode *zith_ast_make_import(ZithArena *a, ZithSourceLoc loc,
                                       ZithImportPayload data) {
    ZithNodeId node_type = data.is_export ? ZITH_NODE_EXPORT : ZITH_NODE_IMPORT;
    ZithNode *n = alloc_node(a, node_type, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithImportPayload>(a, n);
    if (!p) return n;
    *p = data;
    if (data.path)
        p->path = zith_arena_str(a, data.path, data.path_len);
    if (data.alias && data.alias_len)
        p->alias = zith_arena_str(a, data.alias, data.alias_len);
    n->data.list.len = data.path_len;
    return n;
}

// ident.str/len = label name
// list → ZithGotoPayload
ZithNode *zith_ast_make_goto(ZithArena *a, ZithSourceLoc loc,
                                     ZithGotoPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_GOTO, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithGotoPayload>(a, n);
    if (!p) return n;
    *p = data;
    p->target = data.target
                    ? zith_arena_str(a, data.target, data.target_len)
                    : nullptr;
    n->data.list.len = data.arg_count;
    return n;
}

// list → ZithMarkerPayload, list.len = param_count
ZithNode *zith_ast_make_marker(ZithArena *a, ZithSourceLoc loc,
                                       ZithMarkerPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_MARKER, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithMarkerPayload>(a, n);
    if (!p) return n;
    *p = data;
    p->name = data.name ? zith_arena_str(a, data.name, data.name_len) : nullptr;
    n->data.list.len = data.param_count;
    return n;
}

// Identical layout to marker but different node id — anonymous when name = NULL
ZithNode *zith_ast_make_entry(ZithArena *a, ZithSourceLoc loc,
                                      ZithMarkerPayload data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_ENTRY, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithMarkerPayload>(a, n);
    if (!p) return n;
    *p = data;
    p->name = data.name ? zith_arena_str(a, data.name, data.name_len) : nullptr;
    n->data.list.len = data.param_count;
    return n;
}

// label = NULL / len = 0 for plain break
ZithNode *zith_ast_make_break(ZithArena *a, ZithSourceLoc loc,
                                      const char *label, size_t len) {
    ZithNode *n = alloc_node(a, ZITH_NODE_BREAK, loc);
    if (!n) return nullptr;
    if (label && len) {
        n->data.ident.str = zith_arena_strdup(a, label);
        n->data.ident.len = len;
    }
    return n;
}

ZithNode *zith_ast_make_continue(ZithArena *a, ZithSourceLoc loc,
                                         const char *label, size_t len) {
    ZithNode *n = alloc_node(a, ZITH_NODE_CONTINUE, loc);
    if (!n) return nullptr;
    if (label && len) {
        n->data.ident.str = zith_arena_strdup(a, label);
        n->data.ident.len = len;
    }
    return n;
}

// kids.a = body/expr, custom = is_block
ZithNode *zith_ast_make_spawn(ZithArena *a, ZithSourceLoc loc,
                                      ZithNode *body, bool is_block) {
    const ZithNodeId id = is_block ? (uint16_t)ZITH_NODE_SPAWN_STMT : (uint16_t)ZITH_NODE_SPAWN_EXPR;
    ZithNode *n = alloc_node(a, id, loc);
    if (!n) return nullptr;
    n->data.kids.a = body;
    return n;
}

// ident.str/len = error message
ZithNode *zith_ast_make_error(ZithArena *a, ZithSourceLoc loc,
                                      const char *msg) {
    ZithNode *n = alloc_node(a, ZITH_NODE_ERROR, loc);
    if (!n) return nullptr;
    if (msg) {
        n->data.ident.str = zith_arena_strdup(a, msg);
        n->data.ident.len = strlen(msg);
    }
    return n;
}

// ============================================================================
// Walker
// ============================================================================

static void walk_children(ZithNode * n,
                          ZithASTVisitorFn pre, ZithASTVisitorFn post,
                          void*ud);

void zith_ast_walk(ZithNode * root,
                       ZithASTVisitorFn pre, ZithASTVisitorFn post,
                       void*ud) {
    if (!root) return;
    if (pre && !pre(root, ud)) return;
    walk_children(root, pre, post, ud);
    if (post && !post(root, ud)) return;
}

static void walk_node_list(ZithNode **arr, size_t count,
                           ZithASTVisitorFn pre, ZithASTVisitorFn post, void *ud) {
    for (size_t i = 0; i < count; ++i)
        zith_ast_walk(arr[i], pre, post, ud);
}

static void walk_children(ZithNode * n,
                          ZithASTVisitorFn pre, ZithASTVisitorFn post,
                          void*ud) {
    if (!n) return;

    switch (n->type) {
        // kids.a = left, kids.c = right (kids.b aliases list.len — not safe)
        case ZITH_NODE_BINARY_OP:
            zith_ast_walk(n->data.kids.a, pre, post, ud);
            zith_ast_walk(n->data.kids.c, pre, post, ud);
            break;

        // kids.a/b — no op stored here
        case ZITH_NODE_MEMBER:
        case ZITH_NODE_ARROW_CALL:
        case ZITH_NODE_CAST:
            zith_ast_walk(n->data.kids.a, pre, post, ud);
            zith_ast_walk(n->data.kids.b, pre, post, ud);
            break;

        // kids.a only
        case ZITH_NODE_UNARY_OP:
        case ZITH_NODE_RETURN:
        case ZITH_NODE_YIELD:
        case ZITH_NODE_AWAIT_STMT:
        case ZITH_NODE_SPAWN_STMT:
        case ZITH_NODE_SPAWN_EXPR:
            zith_ast_walk(n->data.kids.a, pre, post, ud);
            break;

        // kids.a/b/c
        case ZITH_NODE_IF:
            zith_ast_walk(n->data.kids.a, pre, post, ud); // condition
            zith_ast_walk(n->data.kids.b, pre, post, ud); // then
            zith_ast_walk(n->data.kids.c, pre, post, ud); // else (NULL ok)
            break;

        // list.ptr = ZithNode**, list.len = count
        case ZITH_NODE_PROGRAM:
        case ZITH_NODE_BLOCK:
            walk_node_list(static_cast<ZithNode **>(n->data.list.ptr),
                           n->data.list.len, pre, post, ud);
            break;

        // list → ZithCallPayload
        case ZITH_NODE_CALL:
        case ZITH_NODE_RECURSE: {
            auto *p = static_cast<ZithCallPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->callee, pre, post, ud);
            walk_node_list(p->args, p->arg_count, pre, post, ud);
            break;
        }

        // list → ZithVarPayload
        case ZITH_NODE_VAR_DECL: {
            auto *p = static_cast<ZithVarPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->type_node, pre, post, ud);
            zith_ast_walk(p->initializer, pre, post, ud);
            break;
        }

        // list → ZithFuncPayload
        case ZITH_NODE_FUNC_DECL: {
            auto *p = static_cast<ZithFuncPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->params, p->param_count, pre, post, ud);
            zith_ast_walk(p->return_type, pre, post, ud);
            zith_ast_walk(p->body, pre, post, ud);
            break;
        }

        // list → ZithParamPayload
        case ZITH_NODE_PARAM: {
            auto *p = static_cast<ZithParamPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->type_node, pre, post, ud);
            zith_ast_walk(p->default_value, pre, post, ud);
            break;
        }

        // list → ZithForPayload
        case ZITH_NODE_FOR: {
            auto *p = static_cast<ZithForPayload *>(n->data.list.ptr);
            if (!p) break;
            if (p->is_for_in) {
                zith_ast_walk(p->iterator_var, pre, post, ud);
                zith_ast_walk(p->iterable, pre, post, ud);
            } else {
                zith_ast_walk(p->init, pre, post, ud);
                zith_ast_walk(p->condition, pre, post, ud);
                zith_ast_walk(p->step, pre, post, ud);
            }
            zith_ast_walk(p->body, pre, post, ud);
            break;
        }

        // list → ZithStructPayload
        case ZITH_NODE_STRUCT_DECL: {
            auto *p = static_cast<ZithStructPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->fields, p->field_count, pre, post, ud);
            walk_node_list(p->methods, p->method_count, pre, post, ud);
            break;
        }

        // list → ZithEnumPayload
        case ZITH_NODE_ENUM_DECL: {
            auto *p = static_cast<ZithEnumPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->variants, p->variant_count, pre, post, ud);
            break;
        }

        // list → ZithEnumPayload
        case ZITH_NODE_UNION_DECL: {
            auto *p = static_cast<ZithUnionPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->types, p->type_count, pre, post, ud);
            break;
        }

        // list → ZithSwitchPayload
        case ZITH_NODE_SWITCH: {
            auto *p = static_cast<ZithSwitchPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->subject, pre, post, ud);
            walk_node_list(p->arms, p->arm_count, pre, post, ud);
            zith_ast_walk(p->default_arm, pre, post, ud);
            break;
        }

        // list → ZithTryCatchPayload
        case ZITH_NODE_TRY_CATCH: {
            auto *p = static_cast<ZithTryCatchPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->try_block, pre, post, ud);
            zith_ast_walk(p->catch_block, pre, post, ud);
            break;
        }

        // list → ZithGotoPayload (args are optional)
        case ZITH_NODE_GOTO: {
            auto *p = static_cast<ZithGotoPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->args, p->arg_count, pre, post, ud);
            break;
        }

        // list → ZithEnumVariantPayload
        case ZITH_NODE_ENUM_VARIANT: {
            auto *p = static_cast<ZithEnumVariantPayload *>(n->data.list.ptr);
            if (!p) break;
            zith_ast_walk(p->value, pre, post, ud);
            break;
        }

        // Leaf nodes — no children
        case ZITH_NODE_LITERAL:
        case ZITH_NODE_IDENTIFIER:
        case ZITH_NODE_BREAK:
        case ZITH_NODE_CONTINUE:
        case ZITH_NODE_IMPORT:
        case ZITH_NODE_EXPORT:
        case ZITH_NODE_ERROR:
            break;

        // list → ZithMarkerPayload
        case ZITH_NODE_MARKER:
        case ZITH_NODE_ENTRY: {
            auto *p = static_cast<ZithMarkerPayload *>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->params, p->param_count, pre, post, ud);
            zith_ast_walk(p->body, pre, post, ud);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Debug
// ============================================================================

const char *zith_ast_node_name(ZithNodeId id) {
    switch (id) {
        case ZITH_NODE_ERROR: return "error";
        case ZITH_NODE_LITERAL: return "literal";
        case ZITH_NODE_IDENTIFIER: return "identifier";
        case ZITH_NODE_BINARY_OP: return "binary_op";
        case ZITH_NODE_UNARY_OP: return "unary_op";
        case ZITH_NODE_CALL: return "call";
        case ZITH_NODE_INDEX: return "index";
        case ZITH_NODE_MEMBER: return "member";
        case ZITH_NODE_VAR_DECL: return "var_decl";
        case ZITH_NODE_FUNC_DECL: return "func_decl";
        case ZITH_NODE_PARAM: return "param";
        case ZITH_NODE_BLOCK: return "block";
        case ZITH_NODE_IF: return "if";
        case ZITH_NODE_FOR: return "for";
        case ZITH_NODE_RETURN: return "return";
        case ZITH_NODE_EXPR_STMT: return "expr_stmt";
        case ZITH_NODE_TYPE_REF: return "type_ref";
        case ZITH_NODE_TYPE_FUNC: return "type_func";
        case ZITH_NODE_ARROW_CALL: return "arrow_call";
        case ZITH_NODE_CAST: return "cast";
        case ZITH_NODE_RECURSE: return "recurse";
        case ZITH_NODE_SPAWN_EXPR: return "spawn_expr";
        case ZITH_NODE_PROGRAM: return "program";
        case ZITH_NODE_STRUCT_DECL: return "struct_decl";
        case ZITH_NODE_ENUM_DECL: return "enum_decl";
        case ZITH_NODE_UNION_DECL: return "union_decl";
        case ZITH_NODE_SWITCH: return "switch";
        case ZITH_NODE_CASE: return "case";
        case ZITH_NODE_BREAK: return "break";
        case ZITH_NODE_CONTINUE: return "continue";
        case ZITH_NODE_GOTO: return "goto";
        case ZITH_NODE_MARKER: return "marker";
        case ZITH_NODE_ENTRY: return "entry";
        case ZITH_NODE_TRY_CATCH: return "try_catch";
        case ZITH_NODE_SPAWN_STMT: return "spawn_stmt";
        case ZITH_NODE_AWAIT_STMT: return "await";
        case ZITH_NODE_YIELD: return "yield";
        case ZITH_NODE_IMPORT: return "import";
        case ZITH_NODE_FIELD: return "field";
        case ZITH_NODE_ENUM_VARIANT: return "enum_variant";
        case ZITH_NODE_STRUCT_LIT: return "struct_lit";
        case ZITH_NODE_ARRAY_LIT: return "array_lit";
        default: return "<?>";
    }
}

// list → ZithFieldPayload
ZithNode *zith_ast_make_field(ZithArena *a, ZithSourceLoc loc,
                                      ZithFieldPayload field) {
    ZithNode *n = alloc_node(a, ZITH_NODE_FIELD, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithFieldPayload>(a, n);
    if (!p) return n;
    *p = field;
    p->name = zith_arena_str(a, field.name, field.name_len);
    return n;
}

// list → ZithEnumVariantPayload
// Uses payload to avoid union collision (ident.str and kids.a both at offset 0)
ZithNode *zith_ast_make_enum_variant(ZithArena *a, const ZithSourceLoc loc,
                                             const ZithEnumVariantPayload &data) {
    ZithNode *n = alloc_node(a, ZITH_NODE_ENUM_VARIANT, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithEnumVariantPayload>(a, n);
    if (!p) return n;
    *p = data;
    p->name = zith_arena_str(a, data.name, data.name_len);
    return n;
}

ZithNode *zith_ast_make_union(ZithArena *a, const ZithSourceLoc loc,
                                      const ZithUnionPayload &decl) {
    ZithNode *n = alloc_node(a, ZITH_NODE_UNION_DECL, loc);
    if (!n) return nullptr;
    auto *p = alloc_payload<ZithUnionPayload>(a, n);
    if (!p) return n;
    *p = decl;
    p->name = zith_arena_str(a, decl.name, decl.name_len);
    n->data.list.len = decl.type_count;
    return n;
}

const char *zith_ast_visibility_name(ZithVisibility vis) {
    switch (vis) {
        case ZITH_VIS_PRIVATE: return "private";
        case ZITH_VIS_PUBLIC: return "public";
        case ZITH_VIS_PROTECTED: return "protected";
        default: return "<?>";
    }
}

const char *zith_ast_literal_kind_name(ZithLiteralKind kind) {
    switch (kind) {
        case ZITH_LIT_INT: return "int";
        case ZITH_LIT_UINT: return "uint";
        case ZITH_LIT_FLOAT: return "float";
        case ZITH_LIT_STRING: return "string";
        case ZITH_LIT_BOOL: return "bool";
        default: return "<?>";
    }
}

const char *zith_ast_fn_kind_name(ZithFnKind kind) {
    switch (kind) {
        case ZITH_FN_NORMAL: return "fn";
        case ZITH_FN_ASYNC: return "async fn";
        case ZITH_FN_NORETURN: return "noreturn fn";
        case ZITH_FN_FLOWING: return "flowing fn";
        default: return "<?>";
    }
}

static void print_indent(int d) {
    for (int i = 0; i < d; ++i) debug_print("  ");
}

void zith_ast_print(const ZithNode *node, int indent) {
    if (!node) return;

    print_indent(indent);
    debug_print("[%s] line %zu\n",
            zith_ast_node_name(node->type), node->loc.line);

    switch (node->type) {
        case ZITH_NODE_IDENTIFIER:
            print_indent(indent + 1);
            debug_print("name: %.*s\n",
                    (int) node->data.ident.len, node->data.ident.str);
            break;

        case ZITH_NODE_LITERAL: {
            auto *lit = static_cast<const ZithLiteral *>(node->data.list.ptr);
            if (!lit) {
                print_indent(indent + 1);
                debug_error("(null payload)\n");
                break;
            }
            print_indent(indent + 1);
            debug_print("kind: %s  ", zith_ast_literal_kind_name(lit->kind));
            switch (lit->kind) {
                case ZITH_LIT_INT: debug_print("value: %lld\n", (long long) lit->value.i64); break;
                case ZITH_LIT_UINT: debug_print("value: %llu\n", (unsigned long long) lit->value.u64); break;
                case ZITH_LIT_FLOAT: debug_print("value: %g\n", lit->value.f64); break;
                case ZITH_LIT_BOOL: debug_print("value: %s\n", lit->value.boolean ? "true" : "false"); break;
                case ZITH_LIT_STRING: debug_print("value: \"%.*s\"\n",
                                                  (int) lit->value.string.len, lit->value.string.ptr); break;
                default: debug_print("value: ?\n"); break;
            }
            break;
        }

        case ZITH_NODE_BINARY_OP: {
            const auto op = static_cast<ZithTokenType>(node->data.list.len);
            print_indent(indent + 1);
            debug_print("op: %s (%d)\n", zith_token_type_name(op), (int) op);
            if (node->data.kids.a) zith_ast_print(node->data.kids.a, indent + 2);
            if (node->data.kids.c) zith_ast_print(node->data.kids.c, indent + 2);
            break;
        }

        case ZITH_NODE_UNARY_OP: {
            const auto op = static_cast<ZithTokenType>(node->data.list.len & 0xFFFF);
            const bool is_postfix = (node->data.list.len >> 16) != 0;
            print_indent(indent + 1);
            debug_print("op: %s  postfix: %d\n",
                    zith_token_type_name(op), (int) is_postfix);
            if (node->data.kids.a) zith_ast_print(node->data.kids.a, indent + 2);
            break;
        }

        case ZITH_NODE_FUNC_DECL: {
            auto *p = static_cast<const ZithFuncPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("%s %s  params: %zu  visibility: %s\n",
                    zith_ast_fn_kind_name(p->kind), p->name,
                    p->param_count,
                    zith_ast_visibility_name(p->visibility));
            for (size_t i = 0; i < p->param_count; ++i)
                zith_ast_print(p->params[i], indent + 2);
            zith_ast_print(p->return_type, indent + 2);
            zith_ast_print(p->body, indent + 2);
            break;
        }

        case ZITH_NODE_VAR_DECL: {
            auto *p = static_cast<const ZithVarPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %s  binding: %d  own: %d\n",
                    p->name, (int) p->binding, (int) p->ownership);
            zith_ast_print(p->type_node, indent + 2);
            zith_ast_print(p->initializer, indent + 2);
            break;
        }

        case ZITH_NODE_PROGRAM:
        case ZITH_NODE_BLOCK: {
            auto **stmts = static_cast<ZithNode **>(node->data.list.ptr);
            const size_t count = node->data.list.len;
            print_indent(indent + 1);
            debug_print("children: %zu\n", count);
            for (size_t i = 0; i < count; ++i)
                zith_ast_print(stmts[i], indent + 2);
            break;
        }

        case ZITH_NODE_IF:
            print_indent(indent + 1);
            debug_print("condition:\n");
            if (node->data.kids.a) zith_ast_print(node->data.kids.a, indent + 2);
            print_indent(indent + 1);
            debug_print("then:\n");
            if (node->data.kids.b) zith_ast_print(node->data.kids.b, indent + 2);
            if (node->data.kids.c) {
                print_indent(indent + 1);
                debug_print("else:\n");
                zith_ast_print(node->data.kids.c, indent + 2);
            }
            break;

        case ZITH_NODE_GOTO: {
            auto *p = static_cast<const ZithGotoPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            if (p->is_scene)
                debug_print("target: scene (special)");
            else
                debug_print("target: %.*s", (int) p->target_len, p->target);
            if (p->arg_count)
                debug_print("  args: %zu", p->arg_count);
            debug_print("\n");
            for (size_t i = 0; i < p->arg_count; ++i)
                zith_ast_print(p->args[i], indent + 2);
            break;
        }

        case ZITH_NODE_ENUM_VARIANT: {
            auto *p = static_cast<const ZithEnumVariantPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %.*s\n", (int) p->name_len, p->name);
            if (p->value) zith_ast_print(p->value, indent + 2);
            break;
        }

        case ZITH_NODE_MARKER:
        case ZITH_NODE_ENTRY: {
            auto *p = static_cast<const ZithMarkerPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            if (p->name)
                debug_print("name: %.*s  params: %zu\n",
                        (int) p->name_len, p->name, p->param_count);
            else
                debug_print("(anonymous)  params: %zu\n", p->param_count);
            for (size_t i = 0; i < p->param_count; ++i)
                zith_ast_print(p->params[i], indent + 2);
            zith_ast_print(p->body, indent + 2);
            break;
        }

        case ZITH_NODE_STRUCT_DECL: {
            auto *p = static_cast<const ZithStructPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %s  vis: %s  fields: %zu  methods: %zu\n",
                    p->name, zith_ast_visibility_name(p->visibility),
                    p->field_count, p->method_count);
            for (size_t i = 0; i < p->field_count; ++i) zith_ast_print(p->fields[i], indent + 2);
            for (size_t i = 0; i < p->method_count; ++i) zith_ast_print(p->methods[i], indent + 2);
            break;
        }

        case ZITH_NODE_UNION_DECL: {
            auto *p = static_cast<const ZithUnionPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %s  vis: %s  types: %zu  raw: %d\n",
                    p->name,
                    zith_ast_visibility_name(p->visibility),
                    p->type_count,
                    (int) p->is_raw);
            for (size_t i = 0; i < p->type_count; ++i)
                zith_ast_print(p->types[i], indent + 2);
            break;
        }

        case ZITH_NODE_ENUM_DECL: {
            auto *p = static_cast<const ZithEnumPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %s  vis: %s  variants: %zu\n",
                    p->name, zith_ast_visibility_name(p->visibility),
                    p->variant_count);
            for (size_t i = 0; i < p->variant_count; ++i)
                zith_ast_print(p->variants[i], indent + 2);
            break;
        }

        case ZITH_NODE_FIELD: {
            auto *p = static_cast<const ZithFieldPayload *>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            debug_print("name: %.*s  vis: %s  own: %d\n",
                    (int) p->name_len, p->name,
                    zith_ast_visibility_name(p->visibility),
                    (int) p->ownership);
            if (p->type_node) zith_ast_print(p->type_node, indent + 2);
            if (p->default_value) zith_ast_print(p->default_value, indent + 2);
            break;
        }

        case ZITH_NODE_STRUCT_LIT:
        case ZITH_NODE_ARRAY_LIT: {
            auto **items = static_cast<ZithNode **>(node->data.list.ptr);
            print_indent(indent + 1);
            debug_print("items: %zu\n", node->data.list.len);
            for (size_t i = 0; i < node->data.list.len; ++i)
                zith_ast_print(items[i], indent + 2);
            break;
        }

        case ZITH_NODE_IMPORT: {
            const auto import = static_cast<ZithImportPayload *>(node->data.list.ptr);
            print_indent(indent + 1);
            debug_print("module import path: %s\n", import->path);
            if (import->is_from) {
                print_indent(indent + 2);
                debug_print("(from syntax)\n");
            }
            if (import->alias) {
                print_indent(indent + 2);
                debug_print("alias: %.*s\n", (int)import->alias_len, import->alias);
            }
            break;
        }

        case ZITH_NODE_EXPORT: {
            const auto export_decl = static_cast<ZithImportPayload *>(node->data.list.ptr);
            print_indent(indent + 1);
            debug_print("module export path: %s\n", export_decl->path);
            if (export_decl->alias) {
                print_indent(indent + 2);
                debug_print("alias: %.*s\n", (int)export_decl->alias_len, export_decl->alias);
            }
            break;
        }

        case ZITH_NODE_ERROR:
            print_indent(indent + 1);
            debug_error("msg: %s\n", node->data.ident.str ? node->data.ident.str : "(null)");
            break;

        default:
            break;
    }
}

#ifdef __cplusplus
} // extern "C"
#endif