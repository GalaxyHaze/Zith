// impl/parser/parser_decl.cpp — Top-level declarations, statements, blocks
//
// Refactored to use centralized modules and proper SCAN/PARSE separation.
// In SCAN mode, function bodies and blocks are captured as UNBODY nodes
// without parsing their contents — the parser does NOT analyze block content.
#include "../memory/arena.hpp"
#include "zith/zith.hpp"
#include "parser.h"
#include <cstring>
#include <string>
#include <vector>

using zith::ArenaList;

// Forward declarations — parser utils
extern const ZithToken *parser_peek(const Parser *p);
extern const ZithToken *parser_peek_ahead(const Parser *p, size_t offset);
extern const ZithToken *parser_advance(Parser *p);
extern bool parser_check(const Parser *p, ZithTokenType type);
extern bool parser_match(Parser *p, ZithTokenType type);
extern const ZithToken *parser_expect(Parser *p, ZithTokenType type, const char *msg);
extern void parser_error(Parser *p, ZithSourceLoc loc, const char *msg);
extern void parser_synchronize(Parser *p);
extern bool parser_check_kw(const Parser *p, const char *kw);
extern bool check_kw(const Parser *p, const char *kw);

extern ZithNode *parser_parse_type(Parser *p);
extern ZithNode *parser_parse_expression(Parser *p);

// ============================================================================
// Helpers
// ============================================================================

// Captura tokens entre { e } para criar um nó UNBODY
static ZithNode *capture_unbody(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    
    if (!parser_match(p, ZITH_TOKEN_LBRACE)) {
        // Se não tem '{', retorna nullptr (não é um bloco)
        return nullptr;
    }
    
    // Marca o início dos tokens do corpo (primeiro token após '{')
    const size_t start_pos = p->pos;
    int depth = 1;
    
    // Avança até encontrar o '}' correspondente
    while (!parser_is_at_end(p) && depth > 0) {
        const ZithToken *t = parser_advance(p);
        if (t->type == ZITH_TOKEN_LBRACE) depth++;
        else if (t->type == ZITH_TOKEN_RBRACE) depth--;
    }
    
    // Calcula quantos tokens estão no corpo (excluindo '{' e '}')
    // start_pos aponta para o primeiro token após '{'
    // p->pos agora aponta para o token após '}'
    const size_t token_count = p->pos - start_pos - 1; // -1 para excluir o '}'
    
    // Os tokens do corpo começam em start_pos
    const ZithToken *body_tokens = &p->tokens[start_pos];
    
    return zith_ast_make_unbody(p->arena, loc, body_tokens, token_count);
}

// ============================================================================
// Symbol Registration in SCAN mode
// ============================================================================

struct ScannedSymbolEntry {
    const char *name;
    size_t name_len;
    int kind;
    ZithVisibility visibility;
};

class ScanSymbolCollector {
public:
    static ScanSymbolCollector& instance() {
        static ScanSymbolCollector inst;
        return inst;
    }

    void clear() {
        symbols_.clear();
    }

    void add_function(const char *name, size_t len, ZithVisibility vis) {
        ScannedSymbolEntry entry{name, len, 0, vis};
        symbols_.push_back(entry);
    }

    void add_struct(const char *name, size_t len, ZithVisibility vis) {
        ScannedSymbolEntry entry{name, len, 1, vis};
        symbols_.push_back(entry);
    }

    void add_trait(const char *name, size_t len, ZithVisibility vis) {
        ScannedSymbolEntry entry{name, len, 2, vis};
        symbols_.push_back(entry);
    }

    void add_enum(const char *name, size_t len, ZithVisibility vis) {
        ScannedSymbolEntry entry{name, len, 3, vis};
        symbols_.push_back(entry);
    }

    void add_import(const char *name, size_t len, ZithVisibility vis) {
        ScannedSymbolEntry entry{name, len, 4, vis};
        symbols_.push_back(entry);
    }

    void print_symbols() const {
        size_t n = symbols_.size();
        if (n == 0) { printf("  (no symbols)\n"); return; }
        for (size_t i = 0; i < n; ++i) {
            const auto &sym = symbols_[i];
            const char *kind_str = "???";
            switch (sym.kind) {
                case 0: kind_str = "fn"; break;
                case 1: kind_str = "struct"; break;
                case 2: kind_str = "trait"; break;
                case 3: kind_str = "enum"; break;
                case 4: kind_str = "import"; break;
            }
            const char *vis_str = (sym.visibility == ZITH_VIS_PUBLIC) ? "pub" : "priv";
            printf("  [%s] %.*s (%s)\n", kind_str, (int)sym.name_len, sym.name, vis_str);
        }
    }

    size_t count() const { return symbols_.size(); }
    const ScannedSymbolEntry* data() const { return symbols_.data(); }

private:
    ScanSymbolCollector() = default;
    std::vector<ScannedSymbolEntry> symbols_;
};

void clear_scanned_symbols() {
    ScanSymbolCollector::instance().clear();
}

static void register_fn_symbol(Parser *p, const ZithToken *name_tok, ZithVisibility vis) {
    if (name_tok && p->mode == ZITH_MODE_SCAN) {
        ScanSymbolCollector::instance().add_function(
            name_tok->lexeme.data, name_tok->lexeme.len, vis);
    }
}

static void register_struct_symbol(Parser *p, const ZithToken *name_tok, ZithVisibility vis) {
    if (name_tok && p->mode == ZITH_MODE_SCAN) {
        ScanSymbolCollector::instance().add_struct(
            name_tok->lexeme.data, name_tok->lexeme.len, vis);
    }
}

static void register_import_symbol(Parser *p, const char *name, size_t len, ZithVisibility vis) {
    if (name && p->mode == ZITH_MODE_SCAN) {
        ScanSymbolCollector::instance().add_import(name, len, vis);
    }
}

static ZithVisibility parse_visibility(Parser *p, ZithVisibility *current_vis) {
    ZithVisibility vis = *current_vis;
    if (parser_check(p, ZITH_TOKEN_MODIFIER)) {
        const char *d = parser_peek(p)->lexeme.data;
        size_t l = parser_peek(p)->lexeme.len;
        if (l == 6 && strncmp(d, "public", 6) == 0) vis = ZITH_VIS_PUBLIC;
        else if (l == 9 && strncmp(d, "protected", 9) == 0) vis = ZITH_VIS_PROTECTED;
        else if (l == 7 && strncmp(d, "private", 7) == 0) vis = ZITH_VIS_PRIVATE;
        
        if (parser_peek_ahead(p, 1)->type == ZITH_TOKEN_COLON) {
            parser_advance(p); parser_advance(p); // modifier :
            *current_vis = vis;
            return (ZithVisibility)-1; 
        }
        parser_advance(p);
    }
    return vis;
}

// ============================================================================
// Params & Vars
// ============================================================================

static ZithNode *parse_param(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    ZithOwnership own = ZITH_OWN_DEFAULT;
    bool is_mutable = false;
    if (parser_match(p, ZITH_TOKEN_UNIQUE)) own = ZITH_OWN_UNIQUE;
    else if (parser_match(p, ZITH_TOKEN_SHARED)) own = ZITH_OWN_SHARED;
    else if (parser_match(p, ZITH_TOKEN_VIEW)) own = ZITH_OWN_VIEW;
    else if (parser_match(p, ZITH_TOKEN_LEND)) { own = ZITH_OWN_LEND; is_mutable = true; }
    
    const ZithToken *name = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected param name");
    ZithNode *type_node = nullptr;
    if (parser_check(p, ZITH_TOKEN_COLON)){
        type_node = parser_parse_type(p);
    }
    //parser_expect(p, ZITH_TOKEN_COLON, "expected ':'");
    
    ZithNode *def_val = parser_match(p, ZITH_TOKEN_ASSIGNMENT) ? parser_parse_expression(p) : nullptr;
    
    return zith_ast_make_param(p->arena, loc, {name->lexeme.data, name->lexeme.len, own, type_node, def_val, is_mutable});
}

static ZithNode *parse_var_decl(Parser *p, ZithBindingKind binding) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    ZithOwnership own = ZITH_OWN_DEFAULT;
    if (parser_match(p, ZITH_TOKEN_UNIQUE)) own = ZITH_OWN_UNIQUE;
    else if (parser_match(p, ZITH_TOKEN_SHARED)) own = ZITH_OWN_SHARED;
    else if (parser_match(p, ZITH_TOKEN_VIEW)) own = ZITH_OWN_VIEW;
    else if (parser_match(p, ZITH_TOKEN_LEND)) own = ZITH_OWN_LEND;
    
    const ZithToken *name = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected variable name");
    ZithNode *type_node = parser_match(p, ZITH_TOKEN_COLON) ? parser_parse_type(p) : nullptr;
    
    ZithNode *init = nullptr;
    if (parser_match(p, ZITH_TOKEN_ASSIGNMENT) || parser_match(p, ZITH_TOKEN_DECLARATION)) {
        if (p->mode == ZITH_MODE_SCAN) {
             // SCAN mode: skip the expression to avoid parsing dependencies
             while (!parser_check(p, ZITH_TOKEN_SEMICOLON) && !parser_check(p, ZITH_TOKEN_COMMA) && !parser_is_at_end(p)) parser_advance(p);
        } else {
             init = parser_parse_expression(p);
        }
    }
    parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
    return zith_ast_make_var_decl(p->arena, loc, {name->lexeme.data, name->lexeme.len, binding, own, ZITH_VIS_PRIVATE, type_node, init});
}

// ============================================================================
// Bodies & Statements
// ============================================================================

static ZithNode *parse_body(Parser *p);

ZithNode *parser_parse_statement(Parser *p) {
    if (p->panic) { parser_synchronize(p); p->panic = false; if (parser_is_at_end(p)) return nullptr; }

    /*ZithVisibility vis;
    if (check_kw(p, "public")) vis = ZITH_VIS_PUBLIC; else if (check_kw(p, "protected")) vis = ZITH_VIS_PROTECTED;
    if (parser_peek(p)->type == ZITH_TOKEN_MODIFIER) parser_advance(p);*/

    const ZithSourceLoc loc = parser_peek(p)->loc;
    switch (parser_peek(p)->type) {
        case ZITH_TOKEN_LET: parser_advance(p); return parse_var_decl(p, ZITH_BINDING_LET);
        case ZITH_TOKEN_VAR: parser_advance(p); return parse_var_decl(p, ZITH_BINDING_VAR);
        case ZITH_TOKEN_CONST: parser_advance(p); return parse_var_decl(p, ZITH_BINDING_CONST);
        case ZITH_TOKEN_IF: {
            parser_advance(p);
            ZithNode *cond = parser_parse_expression(p);
            ZithNode *then_b = parse_body(p);
            ZithNode *else_b = parser_match(p, ZITH_TOKEN_ELSE) ? (parser_check(p, ZITH_TOKEN_IF) ? parser_parse_statement(p) : parse_body(p)) : nullptr;
            return zith_ast_make_if(p->arena, loc, cond, then_b, else_b);
        }
        case ZITH_TOKEN_FOR: {
            parser_advance(p);
            ZithForPayload data = {};
            if (parser_check(p, ZITH_TOKEN_IDENTIFIER) && parser_peek_ahead(p, 1)->type == ZITH_TOKEN_IN) {
                data.is_for_in = true; data.iterator_var = parser_parse_expression(p);
                parser_advance(p); // 'in'
                data.iterable = parser_parse_expression(p);
                data.body = parse_body(p);
            } else {
                data.condition = parser_parse_expression(p);
                data.body = parse_body(p);
            }
            return zith_ast_make_for(p->arena, loc, data);
        }
        case ZITH_TOKEN_RETURN: {
            parser_advance(p);
            ZithNode *val = (!parser_check(p, ZITH_TOKEN_SEMICOLON) && !parser_is_at_end(p)) ? parser_parse_expression(p) : nullptr;
            parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
            return zith_ast_make_return(p->arena, loc, val);
        }
        // Nested functions handled by parser_parse_declaration usually, but here for stmt-position
        case ZITH_TOKEN_FN: case ZITH_TOKEN_ASYNC: 
            return parser_parse_declaration(p);
        case ZITH_TOKEN_LBRACE: return parser_parse_block(p);
        default: {
            ZithNode *expr = parser_parse_expression(p);
            if (parser_match(p, ZITH_TOKEN_ASSIGNMENT) || parser_match(p, ZITH_TOKEN_PLUS_EQUAL) || 
                parser_match(p, ZITH_TOKEN_MINUS_EQUAL)) {
                const ZithSourceLoc aloc = parser_peek(p)->loc;
                ZithToken op_t = *parser_peek(p); parser_advance(p);
                expr = zith_ast_make_binary_op(p->arena, aloc, op_t.type, expr, parser_parse_expression(p));
            }
            parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
            return expr;
        }
    }
}

ZithNode *parser_parse_block(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    parser_expect(p, ZITH_TOKEN_LBRACE, "expected '{'");
    ArenaList<ZithNode *> stmts_b; stmts_b.init(p->arena, 16);
    while (!parser_check(p, ZITH_TOKEN_RBRACE) && !parser_is_at_end(p))
        stmts_b.push(p->arena, parser_parse_statement(p));
    parser_expect(p, ZITH_TOKEN_RBRACE, "expected '}'");
    size_t count = 0; ZithNode **stmts = stmts_b.flatten(p->arena, &count);
    return zith_ast_make_block(p->arena, loc, stmts, count);
}

static ZithNode *parse_body(Parser *p) {
    if (parser_match(p, ZITH_TOKEN_LBRACE)) {
        ArenaList<ZithNode *> stmts_b; stmts_b.init(p->arena, 16);
        while (!parser_check(p, ZITH_TOKEN_RBRACE) && !parser_is_at_end(p))
            stmts_b.push(p->arena, parser_parse_statement(p));
        parser_expect(p, ZITH_TOKEN_RBRACE, "expected '}'");
        size_t count = 0; ZithNode **stmts = stmts_b.flatten(p->arena, &count);
        return zith_ast_make_block(p->arena, parser_peek(p)->loc, stmts, count);
    }
    ZithNode *stmt = parser_parse_statement(p);
    if (!stmt) return zith_ast_make_block(p->arena, parser_peek(p)->loc, nullptr, 0);
    ZithNode **arr = (ZithNode**)zith_arena_alloc(p->arena, sizeof(ZithNode*));
    if(arr) *arr = stmt;
    return zith_ast_make_block(p->arena, parser_peek(p)->loc, arr, arr ? 1 : 0);
}

// ============================================================================
// Top-Level Declarations
// ============================================================================

static ZithNode *parse_fn_decl(Parser *p, ZithSourceLoc loc, ZithVisibility vis, bool is_method) {
    ZithFnKind kind = ZITH_FN_NORMAL;
    if (parser_match(p, ZITH_TOKEN_ASYNC)) {  kind = ZITH_FN_ASYNC; }
    else if (parser_match(p, ZITH_TOKEN_FLOWING)) { kind = ZITH_FN_FLOWING; }
    else if (parser_match(p, ZITH_TOKEN_NORETURN)) { kind = ZITH_FN_NORETURN; }
    
    parser_expect(p, ZITH_TOKEN_FN, "expected 'fn' keyword");
    const ZithToken *name = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected function name");
    
    // Register function symbol in SCAN mode
    register_fn_symbol(p, name, vis);
    
    parser_expect(p, ZITH_TOKEN_LPAREN, "expected '('");
    ArenaList<ZithNode *> params_b; 
    params_b.init(p->arena, 8);

    while (!parser_check(p, ZITH_TOKEN_RPAREN) && !parser_is_at_end(p)) {
        params_b.push(p->arena, parse_param(p));
        if (!parser_match(p, ZITH_TOKEN_COMMA)) break;
    }
    parser_expect(p, ZITH_TOKEN_RPAREN, "expected ')'");
    
    ZithNode *ret_type = nullptr;
    if (kind != ZITH_FN_NORETURN && (parser_match(p, ZITH_TOKEN_ARROW) || parser_match(p, ZITH_TOKEN_COLON)))
        ret_type = parser_parse_type(p);

    ZithNode *body = nullptr;
    if (p->mode == ZITH_MODE_SCAN) {
        // SCAN mode: captura o corpo como UNBODY em vez de pular completamente
        if (!parser_match(p, ZITH_TOKEN_COLON)) {
            body = capture_unbody(p);
        }
    } else {
        // PARSE mode: parseia o corpo normalmente
        if (!parser_check(p, ZITH_TOKEN_COLON)) body = parse_body(p);
        else parser_advance(p);
    }
    
    size_t pcount = 0; ZithNode **params = params_b.flatten(p->arena, &pcount);
    return zith_ast_make_func_decl(p->arena, loc, {name->lexeme.data, name->lexeme.len, kind, params, pcount, ret_type, body, vis, is_method});
}

static ZithNode *parse_struct_decl(Parser *p, ZithVisibility struct_vis) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p); // consume 'struct'
    const ZithToken *name = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected struct name");
    
    // Register struct symbol in SCAN mode
    register_struct_symbol(p, name, struct_vis);
    
    parser_expect(p, ZITH_TOKEN_LBRACE, "expected '{'");

    ArenaList<ZithNode *> fields_b, methods_b;
    fields_b.init(p->arena, 8); 
    methods_b.init(p->arena, 4);
    ZithVisibility current_vis = struct_vis;

    while (!parser_check(p, ZITH_TOKEN_RBRACE) && !parser_is_at_end(p)) {
        // 1. Parse Visibility (public/private/protected)
        ZithVisibility item_vis = parse_visibility(p, &current_vis);
        if ((int)item_vis == -1) continue;

        // 2. Check for Methods (fn, async, etc)
        if (parser_check(p, ZITH_TOKEN_FN) || parser_check(p, ZITH_TOKEN_ASYNC) || parser_check(p, ZITH_TOKEN_FLOWING) || parser_check(p, ZITH_TOKEN_NORETURN)) {
            methods_b.push(p->arena, parse_fn_decl(p, parser_peek(p)->loc, item_vis, true));
            continue;
        }
        
        // 3. Check for Fields
        // Matches: x: i32,  or  unique x: i32,  or  let x: i32,
        if (parser_check(p, ZITH_TOKEN_LET) || parser_check(p, ZITH_TOKEN_VAR) ||
            parser_check(p, ZITH_TOKEN_IDENTIFIER) || 
            parser_check(p, ZITH_TOKEN_UNIQUE) || parser_check(p, ZITH_TOKEN_SHARED)) {
            
            // Consume 'let' or 'var' if present (optional in your new syntax)
            if (parser_match(p, ZITH_TOKEN_LET) || parser_match(p, ZITH_TOKEN_VAR)) {}
            
            const ZithSourceLoc floc = parser_peek(p)->loc;
            ZithOwnership own = ZITH_OWN_DEFAULT;
            
            // Parse Ownership (unique/shared)
            if (parser_match(p, ZITH_TOKEN_UNIQUE)) own = ZITH_OWN_UNIQUE;
            else if (parser_match(p, ZITH_TOKEN_SHARED)) own = ZITH_OWN_SHARED;
            
            // Parse Name
            const ZithToken *fname = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected field name");
            
            // Parse Type (Optional: x: i32)
            ZithNode *ftype = nullptr;
            if (parser_match(p, ZITH_TOKEN_COLON)) {
                ftype = parser_parse_type(p);
            }

            // Parse Default Value (Optional: x: i32 = 10)
            ZithNode *fdef = nullptr;
            if (parser_match(p, ZITH_TOKEN_ASSIGNMENT)) {
                if (p->mode == ZITH_MODE_SCAN) {
                    // In scan mode, skip the expression to avoid parsing dependencies
                    while(!parser_check(p, ZITH_TOKEN_COMMA) && !parser_check(p, ZITH_TOKEN_RBRACE) && !parser_is_at_end(p)) {
                        parser_advance(p);
                    }
                } else {
                    fdef = parser_parse_expression(p);
                }
            }
        
            // Use comma as separator, but allow last field to omit it before '}'
            if (parser_peek(p)->type != ZITH_TOKEN_RBRACE)
                parser_expect(p, ZITH_TOKEN_COMMA, "expected ',' or ';'");

            fields_b.push(p->arena, zith_ast_make_field(p->arena, floc, {fname->lexeme.data, fname->lexeme.len, own, item_vis, ftype, fdef}));
            continue;
        }

        // If we get here, we encountered a token we don't understand in the struct body
        parser_error(p, parser_peek(p)->loc, "unexpected token in struct body");
        parser_advance(p); // Skip it to avoid infinite loop
    }
    
    parser_expect(p, ZITH_TOKEN_RBRACE, "expected '}'");

    size_t fc = 0, mc = 0;
    ZithNode **fields = fields_b.flatten(p->arena, &fc);
    ZithNode **methods = methods_b.flatten(p->arena, &mc);
    return zith_ast_make_struct(p->arena, loc, {name->lexeme.data, name->lexeme.len, fields, fc, methods, mc, struct_vis});
}

static ZithNode *parse_import_decl(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);  // consome 'import'
    char buf[256]; size_t buf_len = 0;
    
    // Primeiro segmento (espera IDENTIFIER)
    const ZithToken *seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected module name");
    if (seg->lexeme.len < sizeof(buf)) { memcpy(buf, seg->lexeme.data, seg->lexeme.len); buf_len = seg->lexeme.len; }
    
    // Segmentos adicionais separados por '.' ou '/'
    while (true) {
        if (parser_match(p, ZITH_TOKEN_DOT)) {
            if (buf_len < sizeof(buf) - 1) buf[buf_len++] = '.';
        } else if (parser_match(p, ZITH_TOKEN_DIVIDE)) {
            if (buf_len < sizeof(buf) - 1) buf[buf_len++] = '/';
        } else {
            break;
        }
        seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected identifier after path separator");
        if (buf_len + seg->lexeme.len < sizeof(buf)) {
            memcpy(buf + buf_len, seg->lexeme.data, seg->lexeme.len);
            buf_len += seg->lexeme.len;
        }
    }
    
    // Suporte a alias: import x as y
    const char *alias = nullptr;
    size_t alias_len = 0;
    if (parser_match(p, ZITH_TOKEN_AS)) {
        const ZithToken *alias_tok = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected alias name");
        alias = alias_tok->lexeme.data;
        alias_len = alias_tok->lexeme.len;
    }
    
    parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
    ZithImportPayload payload = {zith_arena_str(p->arena, buf, buf_len), buf_len, ZITH_VIS_PRIVATE, alias, alias_len, false, false};
    register_import_symbol(p, buf, buf_len, ZITH_VIS_PRIVATE);
    return zith_ast_make_import(p->arena, loc, payload);
}

static ZithNode *parse_from_import_decl(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);  // consome 'from'
    
    // Parse do módulo base (ex: std.io.console ou std/io/console)
    char module_buf[256]; size_t module_len = 0;
    const ZithToken *seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected module name");
    if (seg->lexeme.len < sizeof(module_buf)) { memcpy(module_buf, seg->lexeme.data, seg->lexeme.len); module_len = seg->lexeme.len; }
    
    while (true) {
        if (parser_match(p, ZITH_TOKEN_DOT)) {
            if (module_len < sizeof(module_buf) - 1) module_buf[module_len++] = '.';
        } else if (parser_match(p, ZITH_TOKEN_DIVIDE)) {
            if (module_len < sizeof(module_buf) - 1) module_buf[module_len++] = '/';
        } else {
            break;
        }
        seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected identifier after path separator");
        if (module_len + seg->lexeme.len < sizeof(module_buf)) {
            memcpy(module_buf + module_len, seg->lexeme.data, seg->lexeme.len);
            module_len += seg->lexeme.len;
        }
    }
    
    // Espera keyword 'import'
    parser_expect(p, ZITH_TOKEN_IMPORT, "expected 'import' after 'from <module>'");
    
    // Parse dos itens importados (ex: println, println as log)
    char items_buf[256]; //size_t items_len = 0;
    const ZithToken *item = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected item name");
    if (item->lexeme.len < sizeof(items_buf)) { memcpy(items_buf, item->lexeme.data, item->lexeme.len); /*items_len = item->lexeme.len;*/ }
    
    // Suporte a alias: import x as y
    const char *alias = nullptr;
    size_t alias_len = 0;
    if (parser_match(p, ZITH_TOKEN_AS)) {
        const ZithToken *alias_tok = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected alias name");
        alias = alias_tok->lexeme.data;
        alias_len = alias_tok->lexeme.len;
    }
    
    parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
    
    // Path = módulo base, alias = itens importados
    ZithImportPayload payload = {
        zith_arena_str(p->arena, module_buf, module_len), 
        module_len, 
        ZITH_VIS_PRIVATE, 
        alias, 
        alias_len, 
        false,  // is_export = false
        true    // is_from = true
    };
    register_import_symbol(p, module_buf, module_len, ZITH_VIS_PRIVATE);
    return zith_ast_make_import(p->arena, loc, payload);
}

static ZithNode *parse_export_decl(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);  // consome 'export'
    char buf[256]; size_t buf_len = 0;
    
    // Primeiro segmento (espera IDENTIFIER)
    const ZithToken *seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected module name");
    if (seg->lexeme.len < sizeof(buf)) { memcpy(buf, seg->lexeme.data, seg->lexeme.len); buf_len = seg->lexeme.len; }
    
    // Segmentos adicionais separados por '.'
    while (parser_match(p, ZITH_TOKEN_DOT)) {
        if (buf_len < sizeof(buf) - 1) buf[buf_len++] = '.';
        seg = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected identifier after '.'");
        if (buf_len + seg->lexeme.len < sizeof(buf)) {
            memcpy(buf + buf_len, seg->lexeme.data, seg->lexeme.len);
            buf_len += seg->lexeme.len;
        }
    }
    
    parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
    ZithImportPayload payload = {zith_arena_str(p->arena, buf, buf_len), buf_len, ZITH_VIS_PUBLIC, nullptr, 0, true, false};
    register_import_symbol(p, buf, buf_len, ZITH_VIS_PUBLIC);
    return zith_ast_make_import(p->arena, loc, payload);
}

ZithNode *parser_parse_declaration(Parser *p) {
    if (p->panic) { parser_synchronize(p); p->panic = false; if (parser_is_at_end(p)) return nullptr; }

    ZithVisibility vis = parse_visibility(p, &p->current_visibility);
    if ((int)vis == -1) return nullptr;

    const ZithToken *t = parser_peek(p);
    const ZithSourceLoc loc = t->loc;

    if (t->type == ZITH_TOKEN_FN || t->type == ZITH_TOKEN_ASYNC || t->type == ZITH_TOKEN_FLOWING || t->type == ZITH_TOKEN_NORETURN)
        return parse_fn_decl(p, loc, vis, false);

    if (t->type == ZITH_TOKEN_STRUCT) return parse_struct_decl(p, vis);
    if (t->type == ZITH_TOKEN_IMPORT) return parse_import_decl(p);
    if (t->type == ZITH_TOKEN_FROM) return parse_from_import_decl(p);
    if (t->type == ZITH_TOKEN_EXPORT) return parse_export_decl(p);
    if (t->type == ZITH_TOKEN_CONST) { parser_advance(p); return parse_var_decl(p, ZITH_BINDING_CONST); }

    ZithNode *expr = parser_parse_expression(p);
    parser_match(p, ZITH_TOKEN_SEMICOLON);
    return expr;
}

#ifdef __cplusplus
void print_scanned_symbols() {
    printf("Scanned symbols:\n");
    ScanSymbolCollector::instance().print_symbols();
}
#endif