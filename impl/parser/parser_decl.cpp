// impl/parser/parser_decl.cpp
#include "kalidous/kalidous.hpp"
#include "parser.h"
#include "../memory/utils.h"
#include <cstring>

// Forward declarations for utils and expr
extern const KalidousToken *parser_peek(const Parser *p);
extern const KalidousToken *parser_peek_ahead(const Parser *p, size_t offset);
extern const KalidousToken *parser_advance(Parser *p);
extern bool parser_check(const Parser *p, KalidousTokenType type);
extern bool parser_match(Parser *p, KalidousTokenType type);
extern const KalidousToken *parser_expect(Parser *p, KalidousTokenType type, const char *msg);
extern void parser_error(Parser *p, const KalidousSourceLoc loc, const char *msg);
extern void parser_synchronize(Parser *p);
extern bool check_kw(const Parser *p, const char *kw);

extern KalidousNode *parser_parse_type(Parser *p);
extern KalidousNode *parser_parse_expression(Parser *p);

// ============================================================================
// Helpers
// ============================================================================

// Captura tokens entre { e } para criar um nó UNBODY
static KalidousNode *capture_unbody(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    
    if (!parser_match(p, KALIDOUS_TOKEN_LBRACE)) {
        // Se não tem '{', retorna nullptr (não é um bloco)
        return nullptr;
    }
    
    // Marca o início dos tokens do corpo (primeiro token após '{')
    const size_t start_pos = p->pos;
    int depth = 1;
    
    // Avança até encontrar o '}' correspondente
    while (!parser_is_at_end(p) && depth > 0) {
        const KalidousToken *t = parser_advance(p);
        if (t->type == KALIDOUS_TOKEN_LBRACE) depth++;
        else if (t->type == KALIDOUS_TOKEN_RBRACE) depth--;
    }
    
    // Calcula quantos tokens estão no corpo (excluindo '{' e '}')
    // start_pos aponta para o primeiro token após '{'
    // p->pos agora aponta para o token após '}'
    const size_t token_count = p->pos - start_pos - 1; // -1 para excluir o '}'
    
    // Os tokens do corpo começam em start_pos
    const KalidousToken *body_tokens = &p->tokens[start_pos];
    
    return kalidous_ast_make_unbody(p->arena, loc, body_tokens, token_count);
}

static KalidousVisibility parse_visibility(Parser *p, KalidousVisibility *current_vis) {
    KalidousVisibility vis = *current_vis;
    if (parser_check(p, KALIDOUS_TOKEN_MODIFIER)) {
        const char *d = parser_peek(p)->lexeme.data;
        size_t l = parser_peek(p)->lexeme.len;
        if (l == 6 && strncmp(d, "public", 6) == 0) vis = KALIDOUS_VIS_PUBLIC;
        else if (l == 9 && strncmp(d, "protected", 9) == 0) vis = KALIDOUS_VIS_PROTECTED;
        else if (l == 7 && strncmp(d, "private", 7) == 0) vis = KALIDOUS_VIS_PRIVATE;
        
        if (parser_peek_ahead(p, 1)->type == KALIDOUS_TOKEN_COLON) {
            parser_advance(p); parser_advance(p); // modifier :
            *current_vis = vis;
            return (KalidousVisibility)-1; 
        }
        parser_advance(p);
    }
    return vis;
}

// ============================================================================
// Params & Vars
// ============================================================================

static KalidousNode *parse_param(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    KalidousOwnership own = KALIDOUS_OWN_DEFAULT;
    bool is_mutable = false;
    if (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
    else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
    else if (parser_match(p, KALIDOUS_TOKEN_VIEW)) own = KALIDOUS_OWN_VIEW;
    else if (parser_match(p, KALIDOUS_TOKEN_LEND)) { own = KALIDOUS_OWN_LEND; is_mutable = true; }
    
    const KalidousToken *name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected param name");
    parser_expect(p, KALIDOUS_TOKEN_COLON, "expected ':'");
    KalidousNode *type_node = parser_parse_type(p);
    KalidousNode *def_val = parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT) ? parser_parse_expression(p) : nullptr;
    
    return kalidous_ast_make_param(p->arena, loc, {name->lexeme.data, name->lexeme.len, own, type_node, def_val, is_mutable});
}

static KalidousNode *parse_var_decl(Parser *p, KalidousBindingKind binding) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    KalidousOwnership own = KALIDOUS_OWN_DEFAULT;
    if (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
    else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
    else if (parser_match(p, KALIDOUS_TOKEN_VIEW)) own = KALIDOUS_OWN_VIEW;
    else if (parser_match(p, KALIDOUS_TOKEN_LEND)) own = KALIDOUS_OWN_LEND;
    
    const KalidousToken *name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected variable name");
    KalidousNode *type_node = parser_match(p, KALIDOUS_TOKEN_COLON) ? parser_parse_type(p) : nullptr;
    
    KalidousNode *init = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT) || parser_match(p, KALIDOUS_TOKEN_DECLARATION)) {
        if (p->mode == KALIDOUS_MODE_SCAN) {
             // SCAN mode: skip the expression to avoid parsing dependencies
             while (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON) && !parser_check(p, KALIDOUS_TOKEN_COMMA) && !parser_is_at_end(p)) parser_advance(p);
        } else {
             init = parser_parse_expression(p);
        }
    }
    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';'");
    return kalidous_ast_make_var_decl(p->arena, loc, {name->lexeme.data, name->lexeme.len, binding, own, KALIDOUS_VIS_PRIVATE, type_node, init});
}

// ============================================================================
// Bodies & Statements
// ============================================================================

static KalidousNode *parse_body(Parser *p);

KalidousNode *parser_parse_statement(Parser *p) {
    if (p->panic) { parser_synchronize(p); p->panic = false; if (parser_is_at_end(p)) return nullptr; }

    KalidousVisibility vis = KALIDOUS_VIS_PRIVATE;
    if (check_kw(p, "public")) vis = KALIDOUS_VIS_PUBLIC; else if (check_kw(p, "protected")) vis = KALIDOUS_VIS_PROTECTED;
    if (parser_peek(p)->type == KALIDOUS_TOKEN_MODIFIER) parser_advance(p);

    const KalidousSourceLoc loc = parser_peek(p)->loc;
    switch (parser_peek(p)->type) {
        case KALIDOUS_TOKEN_LET: parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_LET);
        case KALIDOUS_TOKEN_VAR: parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_VAR);
        case KALIDOUS_TOKEN_CONST: parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_CONST);
        case KALIDOUS_TOKEN_IF: {
            parser_advance(p);
            KalidousNode *cond = parser_parse_expression(p);
            KalidousNode *then_b = parse_body(p);
            KalidousNode *else_b = parser_match(p, KALIDOUS_TOKEN_ELSE) ? (parser_check(p, KALIDOUS_TOKEN_IF) ? parser_parse_statement(p) : parse_body(p)) : nullptr;
            return kalidous_ast_make_if(p->arena, loc, cond, then_b, else_b);
        }
        case KALIDOUS_TOKEN_FOR: {
            parser_advance(p);
            KalidousForPayload data = {};
            if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER) && parser_peek_ahead(p, 1)->type == KALIDOUS_TOKEN_IN) {
                data.is_for_in = true; data.iterator_var = parser_parse_expression(p);
                parser_advance(p); // 'in'
                data.iterable = parser_parse_expression(p);
                data.body = parse_body(p);
            } else {
                data.condition = parser_parse_expression(p);
                data.body = parse_body(p);
            }
            return kalidous_ast_make_for(p->arena, loc, data);
        }
        case KALIDOUS_TOKEN_RETURN: {
            parser_advance(p);
            KalidousNode *val = (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON) && !parser_is_at_end(p)) ? parser_parse_expression(p) : nullptr;
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';'");
            return kalidous_ast_make_return(p->arena, loc, val);
        }
        // Nested functions handled by parser_parse_declaration usually, but here for stmt-position
        case KALIDOUS_TOKEN_FN: case KALIDOUS_TOKEN_ASYNC: 
            return parser_parse_declaration(p);
        case KALIDOUS_TOKEN_LBRACE: return parser_parse_block(p);
        default: {
            KalidousNode *expr = parser_parse_expression(p);
            if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT) || parser_match(p, KALIDOUS_TOKEN_PLUS_EQUAL) || 
                parser_match(p, KALIDOUS_TOKEN_MINUS_EQUAL)) {
                const KalidousSourceLoc aloc = parser_peek(p)->loc;
                KalidousToken op_t = *parser_peek(p); parser_advance(p);
                expr = kalidous_ast_make_binary_op(p->arena, aloc, op_t.type, expr, parser_parse_expression(p));
            }
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';'");
            return expr;
        }
    }
}

KalidousNode *parser_parse_block(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{'");
    ArenaList<KalidousNode *> stmts_b; stmts_b.init(p->arena, 16);
    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p))
        stmts_b.push(p->arena, parser_parse_statement(p));
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");
    size_t count = 0; KalidousNode **stmts = stmts_b.flatten(p->arena, &count);
    return kalidous_ast_make_block(p->arena, loc, stmts, count);
}

static KalidousNode *parse_body(Parser *p) {
    if (parser_match(p, KALIDOUS_TOKEN_LBRACE)) {
        ArenaList<KalidousNode *> stmts_b; stmts_b.init(p->arena, 16);
        while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p))
            stmts_b.push(p->arena, parser_parse_statement(p));
        parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");
        size_t count = 0; KalidousNode **stmts = stmts_b.flatten(p->arena, &count);
        return kalidous_ast_make_block(p->arena, parser_peek(p)->loc, stmts, count);
    }
    KalidousNode *stmt = parser_parse_statement(p);
    if (!stmt) return kalidous_ast_make_block(p->arena, parser_peek(p)->loc, nullptr, 0);
    KalidousNode **arr = (KalidousNode**)kalidous_arena_alloc(p->arena, sizeof(KalidousNode*));
    if(arr) *arr = stmt;
    return kalidous_ast_make_block(p->arena, parser_peek(p)->loc, arr, arr ? 1 : 0);
}

// ============================================================================
// Top-Level Declarations
// ============================================================================

static KalidousNode *parse_fn_decl(Parser *p, KalidousSourceLoc loc, KalidousVisibility vis, bool is_method) {
    KalidousFnKind kind = KALIDOUS_FN_NORMAL;
    if (check_kw(p, "async")) { parser_advance(p); kind = KALIDOUS_FN_ASYNC; }
    else if (check_kw(p, "flowing")) { parser_advance(p); kind = KALIDOUS_FN_FLOWING; }
    else if (check_kw(p, "noreturn")) { parser_advance(p); kind = KALIDOUS_FN_NORETURN; }
    
    parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' keyword");
    const KalidousToken *name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected function name");
    
    parser_expect(p, KALIDOUS_TOKEN_LPAREN, "expected '('");
    ArenaList<KalidousNode *> params_b; params_b.init(p->arena, 8);
    while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
        params_b.push(p->arena, parse_param(p));
        if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
    }
    parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')'");
    
    KalidousNode *ret_type = nullptr;
    if (kind != KALIDOUS_FN_NORETURN && (parser_match(p, KALIDOUS_TOKEN_ARROW) || parser_match(p, KALIDOUS_TOKEN_COLON)))
        ret_type = parser_parse_type(p);

    KalidousNode *body = nullptr;
    if (p->mode == KALIDOUS_MODE_SCAN) {
        // SCAN mode: captura o corpo como UNBODY em vez de pular completamente
        if (!parser_match(p, KALIDOUS_TOKEN_COLON)) {
            body = capture_unbody(p);
        }
    } else {
        // PARSE mode: parseia o corpo normalmente
        if (!parser_check(p, KALIDOUS_TOKEN_COLON)) body = parse_body(p);
        else parser_advance(p);
    }
    
    size_t pcount = 0; KalidousNode **params = params_b.flatten(p->arena, &pcount);
    return kalidous_ast_make_func_decl(p->arena, loc, {name->lexeme.data, name->lexeme.len, kind, params, pcount, ret_type, body, vis, is_method});
}

static KalidousNode *parse_struct_decl(Parser *p, KalidousVisibility struct_vis) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p); // consume 'struct'
    const KalidousToken *name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected struct name");
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{'");

    ArenaList<KalidousNode *> fields_b, methods_b;
    fields_b.init(p->arena, 8); 
    methods_b.init(p->arena, 4);
    KalidousVisibility current_vis = struct_vis;

    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p)) {
        // 1. Parse Visibility (public/private/protected)
        KalidousVisibility item_vis = parse_visibility(p, &current_vis);
        if ((int)item_vis == -1) continue;

        // 2. Check for Methods (fn, async, etc)
        if (parser_check(p, KALIDOUS_TOKEN_FN) || check_kw(p, "async") || check_kw(p, "flowing") || check_kw(p, "noreturn")) {
            methods_b.push(p->arena, parse_fn_decl(p, parser_peek(p)->loc, item_vis, true));
            continue;
        }
        
        // 3. Check for Fields
        // Matches: x: i32,  or  unique x: i32,  or  let x: i32,
        if (parser_check(p, KALIDOUS_TOKEN_LET) || parser_check(p, KALIDOUS_TOKEN_VAR) ||
            parser_check(p, KALIDOUS_TOKEN_IDENTIFIER) || 
            parser_check(p, KALIDOUS_TOKEN_UNIQUE) || parser_check(p, KALIDOUS_TOKEN_SHARED)) {
            
            // Consume 'let' or 'var' if present (optional in your new syntax)
            if (parser_match(p, KALIDOUS_TOKEN_LET) || parser_match(p, KALIDOUS_TOKEN_VAR)) {}
            
            const KalidousSourceLoc floc = parser_peek(p)->loc;
            KalidousOwnership own = KALIDOUS_OWN_DEFAULT;
            
            // Parse Ownership (unique/shared)
            if (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
            else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
            
            // Parse Name
            const KalidousToken *fname = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected field name");
            
            // Parse Type (Optional: x: i32)
            KalidousNode *ftype = nullptr;
            if (parser_match(p, KALIDOUS_TOKEN_COLON)) {
                ftype = parser_parse_type(p);
            }

            // Parse Default Value (Optional: x: i32 = 10)
            KalidousNode *fdef = nullptr;
            if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT)) {
                if (p->mode == KALIDOUS_MODE_SCAN) {
                    // In scan mode, skip the expression to avoid parsing dependencies
                    while(!parser_check(p, KALIDOUS_TOKEN_COMMA) && !parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p)) {
                        parser_advance(p);
                    }
                } else {
                    fdef = parser_parse_expression(p);
                }
            }
        
            // Use comma as separator, but allow last field to omit it before '}'
            if (parser_peek(p)->type != KALIDOUS_TOKEN_RBRACE)
                parser_expect(p, KALIDOUS_TOKEN_COMMA, "expected ',' or ';'");

            fields_b.push(p->arena, kalidous_ast_make_field(p->arena, floc, {fname->lexeme.data, fname->lexeme.len, own, item_vis, ftype, fdef}));
            continue;
        }

        // If we get here, we encountered a token we don't understand in the struct body
        parser_error(p, parser_peek(p)->loc, "unexpected token in struct body");
        parser_advance(p); // Skip it to avoid infinite loop
    }
    
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");

    size_t fc = 0, mc = 0;
    KalidousNode **fields = fields_b.flatten(p->arena, &fc);
    KalidousNode **methods = methods_b.flatten(p->arena, &mc);
    return kalidous_ast_make_struct(p->arena, loc, {name->lexeme.data, name->lexeme.len, fields, fc, methods, mc, struct_vis});
}

static KalidousNode *parse_import_decl(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    char buf[256]; size_t buf_len = 0;
    const KalidousToken *seg = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected module name");
    if (seg->lexeme.len < sizeof(buf)) { memcpy(buf, seg->lexeme.data, seg->lexeme.len); buf_len = seg->lexeme.len; }
    while (parser_match(p, KALIDOUS_TOKEN_DOT)) {
        if (buf_len < sizeof(buf) - 1) buf[buf_len++] = '.';
        seg = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected identifier after '.'");
        if (buf_len + seg->lexeme.len < sizeof(buf)) {
            memcpy(buf + buf_len, seg->lexeme.data, seg->lexeme.len);
            buf_len += seg->lexeme.len;
        }
    }
    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';'");
    return kalidous_ast_make_import(p->arena, loc, {kalidous_arena_str(p->arena, buf, buf_len), buf_len, KALIDOUS_VIS_PRIVATE});
}

KalidousNode *parser_parse_declaration(Parser *p) {
    if (p->panic) { parser_synchronize(p); p->panic = false; if (parser_is_at_end(p)) return nullptr; }

    KalidousVisibility vis = parse_visibility(p, &p->current_visibility);
    if ((int)vis == -1) return nullptr;

    const KalidousToken *t = parser_peek(p);
    const KalidousSourceLoc loc = t->loc;

    if (t->type == KALIDOUS_TOKEN_FN || check_kw(p, "async") || check_kw(p, "flowing") || check_kw(p, "noreturn"))
        return parse_fn_decl(p, loc, vis, false);
    
    if (t->type == KALIDOUS_TOKEN_STRUCT) return parse_struct_decl(p, vis);
    if (t->type == KALIDOUS_TOKEN_IMPORT) return parse_import_decl(p);
    if (t->type == KALIDOUS_TOKEN_CONST) { parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_CONST); }
    
    KalidousNode *expr = parser_parse_expression(p);
    parser_match(p, KALIDOUS_TOKEN_SEMICOLON);
    return expr;
}