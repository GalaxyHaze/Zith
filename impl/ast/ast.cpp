// impl/parser/kalidous_ast.cpp — AST constructors, walker, and debug
//
// Uses the actual KalidousNode layout from kalidous.h:
//
//   struct KalidousNode {
//       KalidousNodeId    type;
//       KalidousSourceLoc loc;
//       union {
//           struct { KalidousNode* a; KalidousNode* b; KalidousNode* c; } kids;
//           struct { void* ptr; size_t len; }                             list;
//           struct { const char* str; size_t len; }                       ident;
//           struct { double num; }                                         number;
//           struct { bool value; }                                         boolean;
//           uint64_t                                                        custom;
//       } data;
//   };

#include "ast.h"
#include "../utils/debug.h"
#include <cstring>
#include <cstdio>

// ============================================================================
// Internal helpers — C++ linkage only, not exported
// ============================================================================

static KalidousNode* alloc_node(KalidousArena* a, KalidousNodeId id,
                                KalidousSourceLoc loc) {
    auto* n = static_cast<KalidousNode*>(kalidous_arena_alloc(a, sizeof(KalidousNode)));
    if (!n) return nullptr;
    std::memset(n, 0, sizeof(KalidousNode));
    n->type = id;
    n->loc  = loc;
    return n;
}

// Template must be outside extern "C" — C linkage and templates are incompatible
template<typename T>
static T* alloc_payload(KalidousArena* a, KalidousNode* n) {
    auto* p = static_cast<T*>(kalidous_arena_alloc(a, sizeof(T)));
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

KalidousNode* kalidous_ast_make_program(KalidousArena* a,
                                        KalidousNode** decls, size_t count) {
    KalidousSourceLoc root = {0, 1};
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_PROGRAM, root);
    if (!n) return nullptr;
    n->data.list.ptr = decls;
    n->data.list.len = count;
    return n;
}

// list → KalidousLiteral payload
// Strings are interned via kalidous_arena_str (lexemes are not null-terminated)
KalidousNode* kalidous_ast_make_literal(KalidousArena* a, KalidousSourceLoc loc,
                                        KalidousLiteral lit) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_LITERAL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousLiteral>(a, n);
    if (!p) return n;
    *p = lit;
    // Intern the string so the node owns its bytes independent of token lifetime
    if (lit.kind == KALIDOUS_LIT_STRING && lit.value.string.ptr)
        p->value.string.ptr = kalidous_arena_str(a, lit.value.string.ptr,
                                                     lit.value.string.len);
    return n;
}

KalidousNode* kalidous_ast_make_identifier(KalidousArena* a, KalidousSourceLoc loc,
                                           const char* name, size_t len) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_IDENTIFIER, loc);
    if (!n) return nullptr;
    n->data.ident.str = kalidous_arena_strdup(a, name);
    n->data.ident.len = len;
    return n;
}

// Union layout for BINARY_OP:
//   kids.a   (offset  0) = left  operand
//   list.len (offset  8) = operator token type  ← cannot use kids.b here (same offset!)
//   kids.c   (offset 16) = right operand
// kids.b and list.len are at the same offset in the union — using kids.b for right
// and then writing list.len = op would overwrite the right pointer.
KalidousNode* kalidous_ast_make_binary_op(KalidousArena* a, KalidousSourceLoc loc,
                                          KalidousTokenType op,
                                          KalidousNode* left, KalidousNode* right) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_BINARY_OP, loc);
    if (!n) return nullptr;
    n->data.kids.a   = left;
    n->data.list.len = static_cast<size_t>(op);
    n->data.kids.c   = right;  // offset 16 — safe, does not alias list.len
    return n;
}

// Union layout for UNARY_OP:
//   kids.a   (offset  0) = operand
//   list.len (offset  8) = op | (is_postfix << 16)
KalidousNode* kalidous_ast_make_unary_op(KalidousArena* a, KalidousSourceLoc loc,
                                         KalidousTokenType op,
                                         KalidousNode* operand, bool is_postfix) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_UNARY_OP, loc);
    if (!n) return nullptr;
    n->data.kids.a   = operand;
    n->data.list.len = static_cast<size_t>(op) | (static_cast<size_t>(is_postfix) << 16);
    return n;
}

// list → KalidousCallPayload, list.len = arg_count
KalidousNode* kalidous_ast_make_call(KalidousArena* a, KalidousSourceLoc loc,
                                     KalidousNode* callee,
                                     KalidousNode** args, size_t arg_count) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_CALL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousCallPayload>(a, n);
    if (!p) return n;
    p->callee    = callee;
    p->args      = args;
    p->arg_count = arg_count;
    n->data.list.len = arg_count;
    return n;
}

// Same payload as call, different node id — sema validates context
KalidousNode* kalidous_ast_make_recurse(KalidousArena* a, KalidousSourceLoc loc,
                                        KalidousNode* callee,
                                        KalidousNode** args, size_t arg_count) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_RECURSE, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousCallPayload>(a, n);
    if (!p) return n;
    p->callee    = callee;
    p->args      = args;
    p->arg_count = arg_count;
    n->data.list.len = arg_count;
    return n;
}

// kids.a = object, kids.b = member identifier node
KalidousNode* kalidous_ast_make_member(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousNode* object, KalidousNode* member) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_MEMBER, loc);
    if (!n) return nullptr;
    n->data.kids.a = object;
    n->data.kids.b = member;
    return n;
}

// kids.a = receiver, kids.b = call node (KALIDOUS_NODE_CALL)
KalidousNode* kalidous_ast_make_arrow_call(KalidousArena* a, KalidousSourceLoc loc,
                                           KalidousNode* receiver, KalidousNode* call) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_ARROW_CALL, loc);
    if (!n) return nullptr;
    n->data.kids.a = receiver;
    n->data.kids.b = call;
    return n;
}

// kids.a = expr, kids.b = type_node
KalidousNode* kalidous_ast_make_cast(KalidousArena* a, KalidousSourceLoc loc,
                                     KalidousNode* expr, KalidousNode* type_node) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_CAST, loc);
    if (!n) return nullptr;
    n->data.kids.a = expr;
    n->data.kids.b = type_node;
    return n;
}

// list → KalidousVarPayload
KalidousNode* kalidous_ast_make_var_decl(KalidousArena* a, KalidousSourceLoc loc,
                                         KalidousVarPayload decl) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_VAR_DECL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousVarPayload>(a, n);
    if (!p) return n;
    *p      = decl;
    p->name = kalidous_arena_strdup(a, decl.name);
    return n;
}

// list → KalidousFuncPayload, list.len = param_count
KalidousNode* kalidous_ast_make_func_decl(KalidousArena* a, KalidousSourceLoc loc,
                                          KalidousFuncPayload decl) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_FUNC_DECL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousFuncPayload>(a, n);
    if (!p) return n;
    *p           = decl;
    p->name      = kalidous_arena_strdup(a, decl.name);
    n->data.list.len = decl.param_count;
    return n;
}

// list → KalidousParamPayload
KalidousNode* kalidous_ast_make_param(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousParamPayload param) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_PARAM, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousParamPayload>(a, n);
    if (!p) return n;
    *p      = param;
    p->name = kalidous_arena_strdup(a, param.name);
    return n;
}

// list.ptr = KalidousNode**, list.len = count
KalidousNode* kalidous_ast_make_block(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousNode** stmts, size_t count) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_BLOCK, loc);
    if (!n) return nullptr;
    n->data.list.ptr = stmts;
    n->data.list.len = count;
    return n;
}

// kids.a = condition, kids.b = then_branch, kids.c = else_branch (NULL ok)
KalidousNode* kalidous_ast_make_if(KalidousArena* a, KalidousSourceLoc loc,
                                   KalidousNode* cond,
                                   KalidousNode* then_br, KalidousNode* else_br) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_IF, loc);
    if (!n) return nullptr;
    n->data.kids.a = cond;
    n->data.kids.b = then_br;
    n->data.kids.c = else_br;
    return n;
}

// list → KalidousForPayload
KalidousNode* kalidous_ast_make_for(KalidousArena* a, KalidousSourceLoc loc,
                                    KalidousForPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_FOR, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousForPayload>(a, n);
    if (!p) return n;
    *p = data;
    return n;
}

// kids.a = value (NULL = void return)
KalidousNode* kalidous_ast_make_return(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousNode* value) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_RETURN, loc);
    if (!n) return nullptr;
    n->data.kids.a = value;
    return n;
}

// kids.a = value (NULL = bare yield)
KalidousNode* kalidous_ast_make_yield(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousNode* value) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_YIELD, loc);
    if (!n) return nullptr;
    n->data.kids.a = value;
    return n;
}

// kids.a = expr
KalidousNode* kalidous_ast_make_await(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousNode* expr) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_AWAIT_STMT, loc);
    if (!n) return nullptr;
    n->data.kids.a = expr;
    return n;
}

// list → KalidousStructPayload, list.len = field_count
KalidousNode* kalidous_ast_make_struct(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousStructPayload decl) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_STRUCT_DECL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousStructPayload>(a, n);
    if (!p) return n;
    *p           = decl;
    p->name      = kalidous_arena_strdup(a, decl.name);
    n->data.list.len = decl.field_count;
    return n;
}

// list → KalidousEnumPayload, list.len = variant_count
KalidousNode* kalidous_ast_make_enum(KalidousArena* a, KalidousSourceLoc loc,
                                     KalidousEnumPayload decl) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_ENUM_DECL, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousEnumPayload>(a, n);
    if (!p) return n;
    *p           = decl;
    p->name      = kalidous_arena_strdup(a, decl.name);
    n->data.list.len = decl.variant_count;
    return n;
}

// list → KalidousSwitchPayload, list.len = arm_count
KalidousNode* kalidous_ast_make_switch(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousSwitchPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_SWITCH, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousSwitchPayload>(a, n);
    if (!p) return n;
    *p = data;
    n->data.list.len = data.arm_count;
    return n;
}

// list → KalidousTryCatchPayload
KalidousNode* kalidous_ast_make_try_catch(KalidousArena* a, KalidousSourceLoc loc,
                                          KalidousTryCatchPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_TRY_CATCH, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousTryCatchPayload>(a, n);
    if (!p) return n;
    *p = data;
    if (data.catch_var && data.catch_var_len)
        p->catch_var = kalidous_arena_strdup(a, data.catch_var);
    return n;
}

// list → KalidousImportPayload, list.len = path_len
KalidousNode* kalidous_ast_make_import(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousImportPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_IMPORT, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousImportPayload>(a, n);
    if (!p) return n;
    *p = data;
    if (data.alias)
        p->alias = kalidous_arena_strdup(a, data.alias);
    n->data.list.len = data.path_len;
    return n;
}

// ident.str/len = label name
// list → KalidousGotoPayload
KalidousNode* kalidous_ast_make_goto(KalidousArena* a, KalidousSourceLoc loc,
                                     KalidousGotoPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_GOTO, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousGotoPayload>(a, n);
    if (!p) return n;
    *p          = data;
    p->target   = data.target ? kalidous_arena_str(a, data.target, data.target_len)
                               : nullptr;
    n->data.list.len = data.arg_count;
    return n;
}

// list → KalidousMarkerPayload, list.len = param_count
KalidousNode* kalidous_ast_make_marker(KalidousArena* a, KalidousSourceLoc loc,
                                       KalidousMarkerPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_MARKER, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousMarkerPayload>(a, n);
    if (!p) return n;
    *p           = data;
    p->name      = data.name ? kalidous_arena_str(a, data.name, data.name_len) : nullptr;
    n->data.list.len = data.param_count;
    return n;
}

// Identical layout to marker but different node id — anonymous when name = NULL
KalidousNode* kalidous_ast_make_entry(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousMarkerPayload data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_ENTRY, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousMarkerPayload>(a, n);
    if (!p) return n;
    *p           = data;
    p->name      = data.name ? kalidous_arena_str(a, data.name, data.name_len) : nullptr;
    n->data.list.len = data.param_count;
    return n;
}

// label = NULL / len = 0 for plain break
KalidousNode* kalidous_ast_make_break(KalidousArena* a, KalidousSourceLoc loc,
                                      const char* label, size_t len) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_BREAK, loc);
    if (!n) return nullptr;
    if (label && len) {
        n->data.ident.str = kalidous_arena_strdup(a, label);
        n->data.ident.len = len;
    }
    return n;
}

KalidousNode* kalidous_ast_make_continue(KalidousArena* a, KalidousSourceLoc loc,
                                         const char* label, size_t len) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_CONTINUE, loc);
    if (!n) return nullptr;
    if (label && len) {
        n->data.ident.str = kalidous_arena_strdup(a, label);
        n->data.ident.len = len;
    }
    return n;
}

// kids.a = body/expr, custom = is_block
KalidousNode* kalidous_ast_make_spawn(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousNode* body, bool is_block) {
    const KalidousNodeId id = is_block ? KALIDOUS_NODE_SPAWN_STMT : KALIDOUS_NODE_SPAWN_EXPR;
    KalidousNode* n = alloc_node(a, id, loc);
    if (!n) return nullptr;
    n->data.kids.a = body;
    return n;
}

// ident.str/len = error message
KalidousNode* kalidous_ast_make_error(KalidousArena* a, KalidousSourceLoc loc,
                                      const char* msg) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_ERROR, loc);
    if (!n) return nullptr;
    if (msg) {
        n->data.ident.str = kalidous_arena_strdup(a, msg);
        n->data.ident.len = strlen(msg);
    }
    return n;
}

// ============================================================================
// Walker
// ============================================================================

static void walk_children(KalidousNode* n,
                          KalidousASTVisitorFn pre, KalidousASTVisitorFn post,
                          void* ud);

void kalidous_ast_walk(KalidousNode* root,
                       KalidousASTVisitorFn pre, KalidousASTVisitorFn post,
                       void* ud) {
    if (!root) return;
    if (pre  && !pre(root, ud))  return;
    walk_children(root, pre, post, ud);
    if (post && !post(root, ud)) return;
}

static void walk_node_list(KalidousNode** arr, size_t count,
                           KalidousASTVisitorFn pre, KalidousASTVisitorFn post, void* ud) {
    for (size_t i = 0; i < count; ++i)
        kalidous_ast_walk(arr[i], pre, post, ud);
}

static void walk_children(KalidousNode* n,
                          KalidousASTVisitorFn pre, KalidousASTVisitorFn post,
                          void* ud) {
    if (!n) return;

    switch (n->type) {
        // kids.a = left, kids.c = right (kids.b aliases list.len — not safe)
        case KALIDOUS_NODE_BINARY_OP:
            kalidous_ast_walk(n->data.kids.a, pre, post, ud);
            kalidous_ast_walk(n->data.kids.c, pre, post, ud);
            break;

        // kids.a/b — no op stored here
        case KALIDOUS_NODE_MEMBER:
        case KALIDOUS_NODE_ARROW_CALL:
        case KALIDOUS_NODE_CAST:
            kalidous_ast_walk(n->data.kids.a, pre, post, ud);
            kalidous_ast_walk(n->data.kids.b, pre, post, ud);
            break;

        // kids.a only
        case KALIDOUS_NODE_UNARY_OP:
        case KALIDOUS_NODE_RETURN:
        case KALIDOUS_NODE_YIELD:
        case KALIDOUS_NODE_AWAIT_STMT:
        case KALIDOUS_NODE_SPAWN_STMT:
        case KALIDOUS_NODE_SPAWN_EXPR:
            kalidous_ast_walk(n->data.kids.a, pre, post, ud);
            break;

        // kids.a/b/c
        case KALIDOUS_NODE_IF:
            kalidous_ast_walk(n->data.kids.a, pre, post, ud); // condition
            kalidous_ast_walk(n->data.kids.b, pre, post, ud); // then
            kalidous_ast_walk(n->data.kids.c, pre, post, ud); // else (NULL ok)
            break;

        // list.ptr = KalidousNode**, list.len = count
        case KALIDOUS_NODE_PROGRAM:
        case KALIDOUS_NODE_BLOCK:
            walk_node_list(static_cast<KalidousNode**>(n->data.list.ptr),
                           n->data.list.len, pre, post, ud);
            break;

        // list → KalidousCallPayload
        case KALIDOUS_NODE_CALL:
        case KALIDOUS_NODE_RECURSE: {
            auto* p = static_cast<KalidousCallPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->callee, pre, post, ud);
            walk_node_list(p->args, p->arg_count, pre, post, ud);
            break;
        }

        // list → KalidousVarPayload
        case KALIDOUS_NODE_VAR_DECL: {
            auto* p = static_cast<KalidousVarPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->type_node,   pre, post, ud);
            kalidous_ast_walk(p->initializer, pre, post, ud);
            break;
        }

        // list → KalidousFuncPayload
        case KALIDOUS_NODE_FUNC_DECL: {
            auto* p = static_cast<KalidousFuncPayload*>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->params, p->param_count, pre, post, ud);
            kalidous_ast_walk(p->return_type, pre, post, ud);
            kalidous_ast_walk(p->body,        pre, post, ud);
            break;
        }

        // list → KalidousParamPayload
        case KALIDOUS_NODE_PARAM: {
            auto* p = static_cast<KalidousParamPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->type_node,     pre, post, ud);
            kalidous_ast_walk(p->default_value, pre, post, ud);
            break;
        }

        // list → KalidousForPayload
        case KALIDOUS_NODE_FOR: {
            auto* p = static_cast<KalidousForPayload*>(n->data.list.ptr);
            if (!p) break;
            if (p->is_for_in) {
                kalidous_ast_walk(p->iterator_var, pre, post, ud);
                kalidous_ast_walk(p->iterable,     pre, post, ud);
            } else {
                kalidous_ast_walk(p->init,      pre, post, ud);
                kalidous_ast_walk(p->condition, pre, post, ud);
                kalidous_ast_walk(p->step,      pre, post, ud);
            }
            kalidous_ast_walk(p->body, pre, post, ud);
            break;
        }

        // list → KalidousStructPayload
        case KALIDOUS_NODE_STRUCT_DECL: {
            auto* p = static_cast<KalidousStructPayload*>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->fields,  p->field_count,  pre, post, ud);
            walk_node_list(p->methods, p->method_count, pre, post, ud);
            break;
        }

        // list → KalidousEnumPayload
        case KALIDOUS_NODE_ENUM_DECL: {
            auto* p = static_cast<KalidousEnumPayload*>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->variants, p->variant_count, pre, post, ud);
            break;
        }

        // list → KalidousSwitchPayload
        case KALIDOUS_NODE_SWITCH: {
            auto* p = static_cast<KalidousSwitchPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->subject,     pre, post, ud);
            walk_node_list(p->arms, p->arm_count, pre, post, ud);
            kalidous_ast_walk(p->default_arm, pre, post, ud);
            break;
        }

        // list → KalidousTryCatchPayload
        case KALIDOUS_NODE_TRY_CATCH: {
            auto* p = static_cast<KalidousTryCatchPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->try_block,   pre, post, ud);
            kalidous_ast_walk(p->catch_block, pre, post, ud);
            break;
        }

        // list → KalidousGotoPayload (args are optional)
        case KALIDOUS_NODE_GOTO: {
            auto* p = static_cast<KalidousGotoPayload*>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->args, p->arg_count, pre, post, ud);
            break;
        }

        // list → KalidousEnumVariantPayload
        case KALIDOUS_NODE_ENUM_VARIANT: {
            auto* p = static_cast<KalidousEnumVariantPayload*>(n->data.list.ptr);
            if (!p) break;
            kalidous_ast_walk(p->value, pre, post, ud);
            break;
        }

        // Leaf nodes — no children
        case KALIDOUS_NODE_LITERAL:
        case KALIDOUS_NODE_IDENTIFIER:
        case KALIDOUS_NODE_BREAK:
        case KALIDOUS_NODE_CONTINUE:
        case KALIDOUS_NODE_IMPORT:
        case KALIDOUS_NODE_ERROR:
            break;

        // list → KalidousMarkerPayload
        case KALIDOUS_NODE_MARKER:
        case KALIDOUS_NODE_ENTRY: {
            auto* p = static_cast<KalidousMarkerPayload*>(n->data.list.ptr);
            if (!p) break;
            walk_node_list(p->params, p->param_count, pre, post, ud);
            kalidous_ast_walk(p->body, pre, post, ud);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Debug
// ============================================================================

const char* kalidous_ast_node_name(KalidousNodeId id) {
    switch (id) {
        case KALIDOUS_NODE_ERROR:          return "error";
        case KALIDOUS_NODE_LITERAL:        return "literal";
        case KALIDOUS_NODE_IDENTIFIER:     return "identifier";
        case KALIDOUS_NODE_BINARY_OP:      return "binary_op";
        case KALIDOUS_NODE_UNARY_OP:       return "unary_op";
        case KALIDOUS_NODE_CALL:           return "call";
        case KALIDOUS_NODE_INDEX:          return "index";
        case KALIDOUS_NODE_MEMBER:         return "member";
        case KALIDOUS_NODE_VAR_DECL:       return "var_decl";
        case KALIDOUS_NODE_FUNC_DECL:      return "func_decl";
        case KALIDOUS_NODE_PARAM:          return "param";
        case KALIDOUS_NODE_BLOCK:          return "block";
        case KALIDOUS_NODE_IF:             return "if";
        case KALIDOUS_NODE_FOR:            return "for";
        case KALIDOUS_NODE_RETURN:         return "return";
        case KALIDOUS_NODE_EXPR_STMT:      return "expr_stmt";
        case KALIDOUS_NODE_TYPE_REF:       return "type_ref";
        case KALIDOUS_NODE_TYPE_FUNC:      return "type_func";
        case KALIDOUS_NODE_ARROW_CALL:     return "arrow_call";
        case KALIDOUS_NODE_CAST:           return "cast";
        case KALIDOUS_NODE_RECURSE:        return "recurse";
        case KALIDOUS_NODE_SPAWN_EXPR:     return "spawn_expr";
        case KALIDOUS_NODE_PROGRAM:        return "program";
        case KALIDOUS_NODE_STRUCT_DECL:    return "struct_decl";
        case KALIDOUS_NODE_ENUM_DECL:      return "enum_decl";
        case KALIDOUS_NODE_SWITCH:         return "switch";
        case KALIDOUS_NODE_CASE:           return "case";
        case KALIDOUS_NODE_BREAK:          return "break";
        case KALIDOUS_NODE_CONTINUE:       return "continue";
        case KALIDOUS_NODE_GOTO:           return "goto";
        case KALIDOUS_NODE_MARKER:         return "marker";
        case KALIDOUS_NODE_ENTRY:          return "entry";
        case KALIDOUS_NODE_TRY_CATCH:      return "try_catch";
        case KALIDOUS_NODE_SPAWN_STMT:     return "spawn_stmt";
        case KALIDOUS_NODE_AWAIT_STMT:     return "await";
        case KALIDOUS_NODE_YIELD:          return "yield";
        case KALIDOUS_NODE_IMPORT:         return "import";
        case KALIDOUS_NODE_FIELD:          return "field";
        case KALIDOUS_NODE_ENUM_VARIANT:   return "enum_variant";
        case KALIDOUS_NODE_STRUCT_LIT:     return "struct_lit";
        case KALIDOUS_NODE_ARRAY_LIT:      return "array_lit";
        default:                           return "<?>";
    }
}

// list → KalidousFieldPayload
KalidousNode* kalidous_ast_make_field(KalidousArena* a, KalidousSourceLoc loc,
                                      KalidousFieldPayload field) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_FIELD, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousFieldPayload>(a, n);
    if (!p) return n;
    *p      = field;
    p->name = kalidous_arena_str(a, field.name, field.name_len);
    return n;
}

// list → KalidousEnumVariantPayload
// Uses payload to avoid union collision (ident.str and kids.a both at offset 0)
KalidousNode* kalidous_ast_make_enum_variant(KalidousArena* a, const KalidousSourceLoc loc,
                                             const KalidousEnumVariantPayload& data) {
    KalidousNode* n = alloc_node(a, KALIDOUS_NODE_ENUM_VARIANT, loc);
    if (!n) return nullptr;
    auto* p = alloc_payload<KalidousEnumVariantPayload>(a, n);
    if (!p) return n;
    *p       = data;
    p->name  = kalidous_arena_str(a, data.name, data.name_len);
    return n;
}

const char* kalidous_ast_visibility_name(KalidousVisibility vis) {
    switch (vis) {
        case KALIDOUS_VIS_PRIVATE:   return "private";
        case KALIDOUS_VIS_PUBLIC:    return "public";
        case KALIDOUS_VIS_PROTECTED: return "protected";
        default:                     return "<?>";
    }
}

const char* kalidous_ast_literal_kind_name(KalidousLiteralKind kind) {
    switch (kind) {
        case KALIDOUS_LIT_INT:    return "int";
        case KALIDOUS_LIT_UINT:   return "uint";
        case KALIDOUS_LIT_FLOAT:  return "float";
        case KALIDOUS_LIT_STRING: return "string";
        case KALIDOUS_LIT_BOOL:   return "bool";
        default:                  return "<?>";
    }
}

const char* kalidous_ast_fn_kind_name(KalidousFnKind kind) {
    switch (kind) {
        case KALIDOUS_FN_NORMAL:   return "fn";
        case KALIDOUS_FN_ASYNC:    return "async fn";
        case KALIDOUS_FN_NORETURN: return "noreturn fn";
        case KALIDOUS_FN_FLOWING:  return "flowing fn";
        default:                   return "<?>";
    }
}

static void print_indent(int d) {
    for (int i = 0; i < d; ++i) fprintf(stderr, "  ");
}

void kalidous_ast_print(const KalidousNode* node, int indent) {
    if (!node) return;

    print_indent(indent);
    fprintf(stderr, "[%s] line %zu\n",
            kalidous_ast_node_name(node->type), node->loc.line);

    switch (node->type) {
        case KALIDOUS_NODE_IDENTIFIER:
            print_indent(indent + 1);
            fprintf(stderr, "name: %.*s\n",
                    (int)node->data.ident.len, node->data.ident.str);
            break;

        case KALIDOUS_NODE_LITERAL: {
            auto* lit = static_cast<const KalidousLiteral*>(node->data.list.ptr);
            if (!lit) { print_indent(indent + 1); fprintf(stderr, "(null payload)\n"); break; }
            print_indent(indent + 1);
            fprintf(stderr, "kind: %s  ", kalidous_ast_literal_kind_name(lit->kind));
            switch (lit->kind) {
                case KALIDOUS_LIT_INT:    fprintf(stderr, "value: %lld\n",  (long long)lit->value.i64);  break;
                case KALIDOUS_LIT_UINT:   fprintf(stderr, "value: %llu\n",  (unsigned long long)lit->value.u64); break;
                case KALIDOUS_LIT_FLOAT:  fprintf(stderr, "value: %g\n",    lit->value.f64);              break;
                case KALIDOUS_LIT_BOOL:   fprintf(stderr, "value: %s\n",    lit->value.boolean ? "true" : "false"); break;
                case KALIDOUS_LIT_STRING: fprintf(stderr, "value: \"%.*s\"\n",
                                              (int)lit->value.string.len, lit->value.string.ptr); break;
                default: fprintf(stderr, "value: ?\n"); break;
            }
            break;
        }

        case KALIDOUS_NODE_BINARY_OP: {
            const auto op = static_cast<KalidousTokenType>(node->data.list.len);
            print_indent(indent + 1);
            fprintf(stderr, "op: %s (%d)\n", kalidous_token_type_name(op), (int)op);
            if (node->data.kids.a) kalidous_ast_print(node->data.kids.a, indent + 2); // left
            if (node->data.kids.c) kalidous_ast_print(node->data.kids.c, indent + 2); // right
            break;
        }

        case KALIDOUS_NODE_UNARY_OP: {
            const auto op         = static_cast<KalidousTokenType>(node->data.list.len & 0xFFFF);
            const bool is_postfix = (node->data.list.len >> 16) != 0;
            print_indent(indent + 1);
            fprintf(stderr, "op: %s  postfix: %d\n",
                    kalidous_token_type_name(op), (int)is_postfix);
            if (node->data.kids.a) kalidous_ast_print(node->data.kids.a, indent + 2);
            break;
        }

        case KALIDOUS_NODE_FUNC_DECL: {
            auto* p = static_cast<const KalidousFuncPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "%s %s  params: %zu  visibility: %s\n",
                    kalidous_ast_fn_kind_name(p->kind), p->name,
                    p->param_count,
                    kalidous_ast_visibility_name(p->visibility));
            for (size_t i = 0; i < p->param_count; ++i)
                kalidous_ast_print(p->params[i], indent + 2);
            kalidous_ast_print(p->return_type, indent + 2);
            kalidous_ast_print(p->body,        indent + 2);
            break;
        }

        case KALIDOUS_NODE_VAR_DECL: {
            auto* p = static_cast<const KalidousVarPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "name: %s  binding: %d  own: %d\n",
                    p->name, (int)p->binding, (int)p->ownership);
            kalidous_ast_print(p->type_node,   indent + 2);
            kalidous_ast_print(p->initializer, indent + 2);
            break;
        }

        case KALIDOUS_NODE_PROGRAM:
        case KALIDOUS_NODE_BLOCK: {
            auto** stmts = static_cast<KalidousNode**>(node->data.list.ptr);
            const size_t count = node->data.list.len;
            print_indent(indent + 1);
            fprintf(stderr, "children: %zu\n", count);
            for (size_t i = 0; i < count; ++i)
                kalidous_ast_print(stmts[i], indent + 2);
            break;
        }

        case KALIDOUS_NODE_IF:
            print_indent(indent + 1); fprintf(stderr, "condition:\n");
            if (node->data.kids.a) kalidous_ast_print(node->data.kids.a, indent + 2);
            print_indent(indent + 1); fprintf(stderr, "then:\n");
            if (node->data.kids.b) kalidous_ast_print(node->data.kids.b, indent + 2);
            if (node->data.kids.c) {
                print_indent(indent + 1); fprintf(stderr, "else:\n");
                kalidous_ast_print(node->data.kids.c, indent + 2);
            }
            break;

        case KALIDOUS_NODE_GOTO: {
            auto* p = static_cast<const KalidousGotoPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            if (p->is_scene)
                fprintf(stderr, "target: scene (special)");
            else
                fprintf(stderr, "target: %.*s", (int)p->target_len, p->target);
            if (p->arg_count)
                fprintf(stderr, "  args: %zu", p->arg_count);
            fprintf(stderr, "\n");
            for (size_t i = 0; i < p->arg_count; ++i)
                kalidous_ast_print(p->args[i], indent + 2);
            break;
        }

        case KALIDOUS_NODE_ENUM_VARIANT: {
            auto* p = static_cast<const KalidousEnumVariantPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "name: %.*s\n", (int)p->name_len, p->name);
            if (p->value) kalidous_ast_print(p->value, indent + 2);
            break;
        }

        case KALIDOUS_NODE_MARKER:
        case KALIDOUS_NODE_ENTRY: {
            auto* p = static_cast<const KalidousMarkerPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            if (p->name)
                fprintf(stderr, "name: %.*s  params: %zu\n",
                        (int)p->name_len, p->name, p->param_count);
            else
                fprintf(stderr, "(anonymous)  params: %zu\n", p->param_count);
            for (size_t i = 0; i < p->param_count; ++i)
                kalidous_ast_print(p->params[i], indent + 2);
            kalidous_ast_print(p->body, indent + 2);
            break;
        }

        case KALIDOUS_NODE_STRUCT_DECL: {
            auto* p = static_cast<const KalidousStructPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "name: %s  vis: %s  fields: %zu  methods: %zu\n",
                    p->name, kalidous_ast_visibility_name(p->visibility),
                    p->field_count, p->method_count);
            for (size_t i = 0; i < p->field_count;  ++i) kalidous_ast_print(p->fields[i],  indent + 2);
            for (size_t i = 0; i < p->method_count; ++i) kalidous_ast_print(p->methods[i], indent + 2);
            break;
        }

        case KALIDOUS_NODE_ENUM_DECL: {
            auto* p = static_cast<const KalidousEnumPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "name: %s  vis: %s  variants: %zu\n",
                    p->name, kalidous_ast_visibility_name(p->visibility),
                    p->variant_count);
            for (size_t i = 0; i < p->variant_count; ++i)
                kalidous_ast_print(p->variants[i], indent + 2);
            break;
        }

        case KALIDOUS_NODE_FIELD: {
            auto* p = static_cast<const KalidousFieldPayload*>(node->data.list.ptr);
            if (!p) break;
            print_indent(indent + 1);
            fprintf(stderr, "name: %.*s  vis: %s  own: %d\n",
                    (int)p->name_len, p->name,
                    kalidous_ast_visibility_name(p->visibility),
                    (int)p->ownership);
            if (p->type_node)     kalidous_ast_print(p->type_node,     indent + 2);
            if (p->default_value) kalidous_ast_print(p->default_value, indent + 2);
            break;
        }

        case KALIDOUS_NODE_STRUCT_LIT:
        case KALIDOUS_NODE_ARRAY_LIT: {
            auto** items = static_cast<KalidousNode**>(node->data.list.ptr);
            print_indent(indent + 1);
            fprintf(stderr, "items: %zu\n", node->data.list.len);
            for (size_t i = 0; i < node->data.list.len; ++i)
                kalidous_ast_print(items[i], indent + 2);
            break;
        }

        case KALIDOUS_NODE_ERROR:
            print_indent(indent + 1);
            fprintf(stderr, "msg: %s\n", node->data.ident.str ? node->data.ident.str : "(null)");
            break;

        default:
            break;
    }
}

#ifdef __cplusplus
} // extern "C"
#endif