// impl/parser/kalidous_parser.cpp — Recursive descent parser for Kalidous
#include "parser.h"
#include "../memory/utils.h"
#include "../utils/debug.h"
#include <cstring>
#include <cstdio>

// ============================================================================
// Parser init
// ============================================================================

void parser_init(Parser* p, KalidousArena* arena,
                 const char* source, size_t source_len,
                 const char* filename,
                 KalidousTokenStream tokens) {
    p->arena              = arena;
    p->source             = source;
    p->source_len         = source_len;
    p->filename           = filename ? filename : "<input>";
    p->tokens             = tokens.data;
    p->count              = tokens.len;
    p->pos                = 0;
    p->had_error          = false;
    p->panic              = false;
    p->fn_kind            = KALIDOUS_FN_NORMAL;
    p->inside_fn          = false;
    p->current_visibility = KALIDOUS_VIS_PRIVATE;
    p->diags              = { nullptr, 0, 0 };
}

// ============================================================================
// Diagnostics
// ============================================================================

// Find the start and end of line `line_num` (1-based) within source.
// Returns false if the line number is out of range.
static bool find_source_line(const char* source, size_t source_len,
                              size_t line_num,
                              const char** out_start, size_t* out_len) {
    const char* p   = source;
    const char* end = source + source_len;
    size_t      cur = 1;

    while (p < end && cur < line_num) {
        if (*p++ == '\n') cur++;
    }
    if (cur != line_num) return false;

    const char* line_start = p;
    while (p < end && *p != '\n') p++;

    *out_start = line_start;
    *out_len   = (size_t)(p - line_start);
    return true;
}

static const char* severity_label(KalidousDiagSeverity s) {
    switch (s) {
        case KALIDOUS_DIAG_ERROR:   return "error";
        case KALIDOUS_DIAG_WARNING: return "warning";
        case KALIDOUS_DIAG_NOTE:    return "note";
        default:                    return "info";
    }
}

void kalidous_diag_print_all(const KalidousDiagList* diags,
                              const char* source, size_t source_len,
                              const char* filename) {
    if (!diags || diags->count == 0) return;

    for (size_t i = 0; i < diags->count; ++i) {
        const KalidousDiagnostic* d = &diags->items[i];

        // ── Header: file:line:col: severity: message ─────────────────────────
        fprintf(stderr, "%s:%zu:%zu: %s: %s\n",
                filename ? filename : "<input>",
                d->loc.line, d->loc.index,
                severity_label(d->severity),
                d->message);

        // ── Source line ───────────────────────────────────────────────────────
        if (source && source_len > 0) {
            const char* line_ptr = nullptr;
            size_t      line_len = 0;
            if (find_source_line(source, source_len, d->loc.line,
                                 &line_ptr, &line_len)) {
                fprintf(stderr, "  %.*s\n", (int)line_len, line_ptr);

                // ── Caret ─────────────────────────────────────────────────────
                fprintf(stderr, "  ");
                // col is 0-based in KalidousSourceLoc
                const size_t col = d->loc.index;
                for (size_t c = 0; c < col && c < line_len; ++c)
                    fputc(line_ptr[c] == '\t' ? '\t' : ' ', stderr);
                fprintf(stderr, "^\n");
            }
        }
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    size_t errors = 0, warnings = 0;
    for (size_t i = 0; i < diags->count; ++i) {
        if (diags->items[i].severity == KALIDOUS_DIAG_ERROR)   errors++;
        if (diags->items[i].severity == KALIDOUS_DIAG_WARNING) warnings++;
    }
    if (errors > 0 || warnings > 0) {
        fprintf(stderr, "\n%s: ", filename ? filename : "<input>");
        if (errors)   fprintf(stderr, "%zu error(s)",   errors);
        if (errors && warnings) fprintf(stderr, ", ");
        if (warnings) fprintf(stderr, "%zu warning(s)", warnings);
        fprintf(stderr, "\n\n");
    }
}

// ── Internal emit ────────────────────────────────────────────────────────────

static void parser_emit(Parser* p, KalidousSourceLoc loc,
                         KalidousDiagSeverity severity, const char* msg) {
    // Grow the diagnostic array if needed (arena-allocated, no free)
    if (p->diags.count >= p->diags.capacity) {
        const size_t new_cap = p->diags.capacity == 0 ? 8 : p->diags.capacity * 2;
        auto* buf = static_cast<KalidousDiagnostic*>(
            kalidous_arena_alloc(p->arena, new_cap * sizeof(KalidousDiagnostic)));
        if (!buf) return;
        if (p->diags.items)
            memcpy(buf, p->diags.items, p->diags.count * sizeof(KalidousDiagnostic));
        p->diags.items    = buf;
        p->diags.capacity = new_cap;
    }

    KalidousDiagnostic d;
    d.message  = kalidous_arena_strdup(p->arena, msg);
    d.loc      = loc;
    d.severity = severity;
    p->diags.items[p->diags.count++] = d;

    if (severity == KALIDOUS_DIAG_ERROR) p->had_error = true;
}

void parser_error(Parser* p, KalidousSourceLoc loc, const char* msg) {
    if (p->panic) return; // suppress cascades — wait for sync at boundary
    parser_emit(p, loc, KALIDOUS_DIAG_ERROR, msg);
}
void parser_warning(Parser* p, KalidousSourceLoc loc, const char* msg) {
    parser_emit(p, loc, KALIDOUS_DIAG_WARNING, msg);
}
void parser_note   (Parser* p, KalidousSourceLoc loc, const char* msg) {
    parser_emit(p, loc, KALIDOUS_DIAG_NOTE, msg);
}


// ============================================================================
// Error recovery — panic mode
// ============================================================================

// Returns true if the current token is a safe point to resume parsing.
// Called after emitting an error to skip past the broken tokens.
static bool is_sync_point(const Parser* p) {
    switch (parser_peek(p)->type) {
        // Statement boundaries
        case KALIDOUS_TOKEN_SEMICOLON:
        // Block boundaries
        case KALIDOUS_TOKEN_LBRACE:
        case KALIDOUS_TOKEN_RBRACE:
        // Top-level declarations
        case KALIDOUS_TOKEN_FN:
        case KALIDOUS_TOKEN_ASYNC:
        case KALIDOUS_TOKEN_STRUCT:
        case KALIDOUS_TOKEN_ENUM:
        case KALIDOUS_TOKEN_TRAIT:
        case KALIDOUS_TOKEN_IMPLEMENT:
        case KALIDOUS_TOKEN_COMPONENT:
        case KALIDOUS_TOKEN_UNION:
        case KALIDOUS_TOKEN_FAMILY:
        case KALIDOUS_TOKEN_ENTITY:
        case KALIDOUS_TOKEN_MODIFIER:
        // Statement keywords
        case KALIDOUS_TOKEN_IF:
        case KALIDOUS_TOKEN_FOR:
        case KALIDOUS_TOKEN_RETURN:
        case KALIDOUS_TOKEN_YIELD:
        case KALIDOUS_TOKEN_GOTO:
        case KALIDOUS_TOKEN_MARKER:
        case KALIDOUS_TOKEN_ENTRY:
        case KALIDOUS_TOKEN_LET:
        case KALIDOUS_TOKEN_VAR:
        case KALIDOUS_TOKEN_CONST:
        // Always stop at EOF
        case KALIDOUS_TOKEN_END:
            return true;
        default:
            return false;
    }
}

// Advance past broken tokens until a sync point is found.
// Does NOT consume the sync token itself — the caller's loop will handle it.
static void parser_synchronize(Parser* p) {
    while (!parser_is_at_end(p)) {
        // If the token we just consumed was ';', the next token is a clean start
        if (parser_peek(p)->type == KALIDOUS_TOKEN_SEMICOLON) {
            parser_advance(p); // consume the ';'
            return;
        }
        if (is_sync_point(p)) return;
        parser_advance(p);
    }
}



const KalidousToken* parser_peek(const Parser* p) {
    if (p->pos < p->count) return &p->tokens[p->pos];
    static constexpr KalidousToken eof = { {nullptr, 0}, {0, 0}, KALIDOUS_TOKEN_END, 0 };
    return &eof;
}

const KalidousToken* parser_peek_ahead(const Parser* p, size_t offset) {
    const size_t idx = p->pos + offset;
    if (idx < p->count) return &p->tokens[idx];
    static constexpr KalidousToken eof = { {nullptr, 0}, {0, 0}, KALIDOUS_TOKEN_END, 0 };
    return &eof;
}

const KalidousToken* parser_advance(Parser* p) {
    const KalidousToken* t = parser_peek(p);
    if (p->pos < p->count) p->pos++;
    return t;
}

bool parser_check(const Parser* p, KalidousTokenType type) {
    return parser_peek(p)->type == type;
}

bool parser_match(Parser* p, KalidousTokenType type) {
    if (parser_check(p, type)) { parser_advance(p); return true; }
    return false;
}

const KalidousToken* parser_expect(Parser* p, KalidousTokenType type, const char* msg) {
    if (parser_check(p, type)) return parser_advance(p);

    // In panic mode, suppress cascading errors — just return without emitting
    if (p->panic) return parser_peek(p);

    const KalidousToken* t = parser_peek(p);
    char buf[256];
    if (t->lexeme.data && t->lexeme.len > 0) {
        snprintf(buf, sizeof(buf), "%s (got '%.*s' — %s)",
                 msg, (int)t->lexeme.len, t->lexeme.data,
                 kalidous_token_type_name(t->type));
    } else {
        snprintf(buf, sizeof(buf), "%s (got %s)",
                 msg, kalidous_token_type_name(t->type));
    }
    parser_error(p, t->loc, buf);
    p->panic = true; // synchronization happens at statement/declaration boundary
    return t;
}

bool parser_is_at_end(const Parser* p) {
    return parser_peek(p)->type == KALIDOUS_TOKEN_END;
}

static bool check_kw(const Parser* p, const char* kw) {
    const KalidousToken* t = parser_peek(p);
    if (t->type != KALIDOUS_TOKEN_IDENTIFIER) return false;
    const size_t len = strlen(kw);
    return t->lexeme.len == len && memcmp(t->lexeme.data, kw, len) == 0;
}

// ============================================================================
// Literal value parsing
// ============================================================================

static bool lexeme_to_cstr(char* buf, size_t buf_size,
                            const char* data, size_t len) {
    if (len >= buf_size) return false;
    memcpy(buf, data, len);
    buf[len] = '\0';
    return true;
}

static KalidousLiteral parse_lit_decimal(const char* data, size_t len) {
    char buf[64];
    if (!lexeme_to_cstr(buf, sizeof(buf), data, len))
        return { KALIDOUS_LIT_INT, { .i64 = 0 } };
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '.') {
            KalidousLiteral lit;
            lit.kind      = KALIDOUS_LIT_FLOAT;
            lit.value.f64 = strtod(buf, nullptr);
            return lit;
        }
    }
    KalidousLiteral lit;
    lit.kind      = KALIDOUS_LIT_INT;
    lit.value.i64 = (int64_t)strtoll(buf, nullptr, 10);
    return lit;
}

static KalidousLiteral parse_lit_hex(const char* data, size_t len) {
    char buf[64];
    lexeme_to_cstr(buf, sizeof(buf), data, len);
    KalidousLiteral lit;
    lit.kind      = KALIDOUS_LIT_UINT;
    lit.value.u64 = (uint64_t)strtoull(buf, nullptr, 16);
    return lit;
}

static KalidousLiteral parse_lit_binary(const char* data, size_t len) {
    const char* digits = (len > 2) ? data + 2 : data;
    size_t      dlen   = (len > 2) ? len - 2 : len;
    uint64_t    val    = 0;
    for (size_t i = 0; i < dlen; ++i) {
        if (digits[i] == '0' || digits[i] == '1')
            val = (val << 1) | (uint64_t)(digits[i] - '0');
    }
    return { KALIDOUS_LIT_UINT, { .u64 = val } };
}

static KalidousLiteral parse_lit_octal(const char* data, size_t len) {
    char buf[64];
    const char* src  = (len > 2) ? data + 2 : data;
    size_t      slen = (len > 2) ? len - 2 : len;
    lexeme_to_cstr(buf, sizeof(buf), src, slen);
    KalidousLiteral lit;
    lit.kind      = KALIDOUS_LIT_UINT;
    lit.value.u64 = (uint64_t)strtoull(buf, nullptr, 8);
    return lit;
}

static KalidousLiteral make_lit_string(const char* data, size_t len) {
    if (len >= 2 && data[0] == '"' && data[len - 1] == '"') {
        data += 1;
        len  -= 2;
    }
    KalidousLiteral lit;
    lit.kind             = KALIDOUS_LIT_STRING;
    lit.value.string.ptr = data;
    lit.value.string.len = len;
    return lit;
}

// ============================================================================
// Types
// ============================================================================

// Parse a block or single-statement body — braces are optional for one-liners.
//   if cond { stmt }   → block
//   if cond stmt;      → synthetic block with one child
// Guards against null statement from error recovery.
static KalidousNode* parse_body(Parser* p) {
    if (parser_check(p, KALIDOUS_TOKEN_LBRACE))
        return parser_parse_block(p);

    const KalidousSourceLoc loc  = parser_peek(p)->loc;
    KalidousNode*           stmt = parser_parse_statement(p);

    // Guard: error recovery can produce nullptr — wrap only if valid
    if (!stmt) return kalidous_ast_make_block(p->arena, loc, nullptr, 0);

    KalidousNode** arr = static_cast<KalidousNode**>(
        kalidous_arena_alloc(p->arena, sizeof(KalidousNode*)));
    if (arr) *arr = stmt;
    return kalidous_ast_make_block(p->arena, loc, arr, arr ? 1 : 0);
}

KalidousNode* parser_parse_type(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;

    if (parser_match(p, KALIDOUS_TOKEN_UNIQUE) ||
        parser_match(p, KALIDOUS_TOKEN_SHARED) ||
        parser_match(p, KALIDOUS_TOKEN_VIEW)   ||
        parser_match(p, KALIDOUS_TOKEN_LEND)) {
        // TODO: wrap result in KALIDOUS_NODE_TYPE_UNIQUE/SHARED/VIEW/LEND
    }

    if (parser_match(p, KALIDOUS_TOKEN_MULTIPLY)) {
        KalidousNode* inner = parser_parse_type(p);
        // TODO: KALIDOUS_NODE_TYPE_POINTER
        return inner;
    }

    if (parser_match(p, KALIDOUS_TOKEN_LBRACKET)) {
        KalidousNode* size_expr = nullptr;
        if (!parser_check(p, KALIDOUS_TOKEN_RBRACKET))
            size_expr = parser_parse_expression(p);
        parser_expect(p, KALIDOUS_TOKEN_RBRACKET, "expected ']' in array type");
        KalidousNode* inner = parser_parse_type(p);
        (void)size_expr;
        // TODO: KALIDOUS_NODE_TYPE_ARRAY
        return inner;
    }

    if (parser_check(p, KALIDOUS_TOKEN_TYPE) || parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
        const KalidousToken* t = parser_advance(p);
        KalidousNode* base = kalidous_ast_make_identifier(p->arena, loc,
                                                          t->lexeme.data, t->lexeme.len);
        if (parser_match(p, KALIDOUS_TOKEN_QUESTION)) {
            // TODO: KALIDOUS_NODE_TYPE_OPTIONAL
        } else if (parser_match(p, KALIDOUS_TOKEN_BANG)) {
            // TODO: KALIDOUS_NODE_TYPE_RESULT
        }
        return base;
    }

    parser_error(p, loc, "expected type");
    return kalidous_ast_make_error(p->arena, loc, "expected type");
}

// ============================================================================
// Expressions — Pratt parser
// ============================================================================

typedef struct { int left; int right; } BindingPower;

static BindingPower infix_bp(KalidousTokenType op) {
    switch (op) {
        case KALIDOUS_TOKEN_OR:                    return {1, 2};
        case KALIDOUS_TOKEN_AND:                   return {3, 4};
        case KALIDOUS_TOKEN_EQUAL:
        case KALIDOUS_TOKEN_NOT_EQUAL:             return {5, 6};
        case KALIDOUS_TOKEN_LESS_THAN:
        case KALIDOUS_TOKEN_GREATER_THAN:
        case KALIDOUS_TOKEN_LESS_THAN_OR_EQUAL:
        case KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL: return {7, 8};
        case KALIDOUS_TOKEN_PLUS:
        case KALIDOUS_TOKEN_MINUS:                 return {9, 10};
        case KALIDOUS_TOKEN_MULTIPLY:
        case KALIDOUS_TOKEN_DIVIDE:
        case KALIDOUS_TOKEN_MOD:                   return {11, 12};
        case KALIDOUS_TOKEN_ARROW:                 return {13, 14};
        case KALIDOUS_TOKEN_DOT:                   return {15, 16};
        default:                                   return {-1, -1};
    }
}

static KalidousNode* parse_expr_bp(Parser* p, int min_bp);

static KalidousNode* parse_nud(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    const KalidousToken* t = parser_advance(p);

    switch (t->type) {

        case KALIDOUS_TOKEN_NUMBER:
            return kalidous_ast_make_literal(p->arena, loc,
                       parse_lit_decimal(t->lexeme.data, t->lexeme.len));
        case KALIDOUS_TOKEN_FLOAT:
            return kalidous_ast_make_literal(p->arena, loc,
                       parse_lit_decimal(t->lexeme.data, t->lexeme.len));
        case KALIDOUS_TOKEN_STRING:
            return kalidous_ast_make_literal(p->arena, loc,
                       make_lit_string(t->lexeme.data, t->lexeme.len));
        case KALIDOUS_TOKEN_HEXADECIMAL:
            return kalidous_ast_make_literal(p->arena, loc,
                       parse_lit_hex(t->lexeme.data, t->lexeme.len));
        case KALIDOUS_TOKEN_BINARY:
            return kalidous_ast_make_literal(p->arena, loc,
                       parse_lit_binary(t->lexeme.data, t->lexeme.len));
        case KALIDOUS_TOKEN_OCTAL:
            return kalidous_ast_make_literal(p->arena, loc,
                       parse_lit_octal(t->lexeme.data, t->lexeme.len));

        case KALIDOUS_TOKEN_IDENTIFIER: {
            KalidousNode* ident = kalidous_ast_make_identifier(p->arena, loc,
                                      t->lexeme.data, t->lexeme.len);
            if (!parser_check(p, KALIDOUS_TOKEN_LPAREN)) return ident;
            parser_advance(p);
            ArenaList<KalidousNode*> args_b;
            args_b.init(p->arena, 8);
            while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                args_b.push(p->arena, parser_parse_expression(p));
                if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
            }
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after arguments");
            size_t         arg_count = 0;
            KalidousNode** args      = args_b.flatten(p->arena, &arg_count);
            return kalidous_ast_make_call(p->arena, loc, ident, args, arg_count);
        }

        case KALIDOUS_TOKEN_RECURSE: {
            if (!p->inside_fn)
                parser_error(p, loc, "'recurse' outside function");
            KalidousNode* self_ref = kalidous_ast_make_identifier(p->arena, loc,
                                         t->lexeme.data, t->lexeme.len);
            parser_expect(p, KALIDOUS_TOKEN_LPAREN, "expected '(' after recurse");
            ArenaList<KalidousNode*> args_b;
            args_b.init(p->arena, 8);
            while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                args_b.push(p->arena, parser_parse_expression(p));
                if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
            }
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after recurse arguments");
            size_t         arg_count = 0;
            KalidousNode** args      = args_b.flatten(p->arena, &arg_count);
            return kalidous_ast_make_recurse(p->arena, loc, self_ref, args, arg_count);
        }

        case KALIDOUS_TOKEN_MINUS:
            return kalidous_ast_make_unary_op(p->arena, loc, KALIDOUS_TOKEN_MINUS,
                                              parse_expr_bp(p, 13), false);
        case KALIDOUS_TOKEN_BANG:
            return kalidous_ast_make_unary_op(p->arena, loc, KALIDOUS_TOKEN_BANG,
                                              parse_expr_bp(p, 13), false);

        case KALIDOUS_TOKEN_LPAREN: {
            KalidousNode* expr = parser_parse_expression(p);
            // TODO: distinguish grouping from tuple (a, b, c)
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')'");
            return expr;
        }

        // Positional struct/array literal: { expr, expr, ... }
        // Comma-separated expressions inside braces — distinct from a block (uses ';')
        case KALIDOUS_TOKEN_LBRACE: {
            ArenaList<KalidousNode*> items_b;
            items_b.init(p->arena, 4);
            while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p)) {
                items_b.push(p->arena, parser_parse_expression(p));
                if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
            }
            parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}' after struct literal");
            size_t         count = 0;
            KalidousNode** items = items_b.flatten(p->arena, &count);
            KalidousNode*  n     = kalidous_ast_make_block(p->arena, loc, items, count);
            if (n) n->type = KALIDOUS_NODE_STRUCT_LIT;
            return n;
        }

        case KALIDOUS_TOKEN_SPAWN: {
            KalidousNode* expr = parser_parse_expression(p);
            return kalidous_ast_make_spawn(p->arena, loc, expr, false);
        }

        default: {
            char buf[128];
            snprintf(buf, sizeof(buf), "unexpected token '%.*s' in expression",
                     (int)t->lexeme.len, t->lexeme.data);
            parser_error(p, loc, buf);
            return kalidous_ast_make_error(p->arena, loc, buf);
        }
    }
}

static KalidousNode* parse_expr_bp(Parser* p, const int min_bp) {
    KalidousNode* left = parse_nud(p);

    while (true) {
        const KalidousTokenType op = parser_peek(p)->type;
        const BindingPower      bp = infix_bp(op);
        if (bp.left < min_bp) break;

        const KalidousSourceLoc loc = parser_peek(p)->loc;
        parser_advance(p);

        if (op == KALIDOUS_TOKEN_DOT) {
            const KalidousToken* member = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                                        "expected member name after '.'");
            KalidousNode* rhs = kalidous_ast_make_identifier(p->arena, member->loc,
                                    member->lexeme.data, member->lexeme.len);
            left = kalidous_ast_make_member(p->arena, loc, left, rhs);

            // a.b(args) — method call
            if (parser_check(p, KALIDOUS_TOKEN_LPAREN)) {
                const KalidousSourceLoc call_loc = parser_peek(p)->loc;
                parser_advance(p);
                ArenaList<KalidousNode*> args_b;
                args_b.init(p->arena, 8);
                while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    args_b.push(p->arena, parser_parse_expression(p));
                    if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                }
                parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after method arguments");
                size_t         arg_count = 0;
                KalidousNode** args      = args_b.flatten(p->arena, &arg_count);
                left = kalidous_ast_make_call(p->arena, call_loc, left, args, arg_count);
            }
            continue;
        }
        if (op == KALIDOUS_TOKEN_ARROW) {
            KalidousNode* rhs = parse_expr_bp(p, bp.right);
            left = kalidous_ast_make_arrow_call(p->arena, loc, left, rhs);
            continue;
        }

        KalidousNode* right = parse_expr_bp(p, bp.right);
        left = kalidous_ast_make_binary_op(p->arena, loc, op, left, right);
    }

    while (true) {
        const KalidousSourceLoc loc = parser_peek(p)->loc;
        if (parser_match(p, KALIDOUS_TOKEN_QUESTION)) {
            left = kalidous_ast_make_unary_op(p->arena, loc,
                                              KALIDOUS_TOKEN_QUESTION, left, true);
        } else if (parser_match(p, KALIDOUS_TOKEN_BANG)) {
            left = kalidous_ast_make_unary_op(p->arena, loc,
                                              KALIDOUS_TOKEN_BANG, left, true);
        } else {
            // TODO: 'as' cast
            break;
        }
    }

    return left;
}

KalidousNode* parser_parse_expression(Parser* p) {
    return parse_expr_bp(p, 0);
}

// ============================================================================
// Statements
// ============================================================================

static KalidousNode* parse_var_decl(Parser* p, KalidousBindingKind binding) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;

    KalidousOwnership own = KALIDOUS_OWN_DEFAULT;
    if      (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
    else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
    else if (parser_match(p, KALIDOUS_TOKEN_VIEW))   own = KALIDOUS_OWN_VIEW;
    else if (parser_match(p, KALIDOUS_TOKEN_LEND))   own = KALIDOUS_OWN_LEND;

    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected variable name");

    KalidousNode* type_node = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_COLON))
        type_node = parser_parse_type(p);

    KalidousNode* initializer = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT) ||
        parser_match(p, KALIDOUS_TOKEN_DECLARATION))
        initializer = parser_parse_expression(p);

    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after variable declaration");

    const KalidousVarPayload decl = {
        name->lexeme.data, name->lexeme.len,
        binding, own, KALIDOUS_VIS_PRIVATE,
        type_node, initializer
    };
    return kalidous_ast_make_var_decl(p->arena, loc, decl);
}

static KalidousNode* parse_if(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    KalidousNode* cond    = parser_parse_expression(p);
    KalidousNode* then_br = parse_body(p);
    KalidousNode* else_br = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ELSE)) {
        else_br = parser_check(p, KALIDOUS_TOKEN_IF)
            ? parse_if(p)
            : parse_body(p);
    }
    return kalidous_ast_make_if(p->arena, loc, cond, then_br, else_br);
}

static KalidousNode* parse_for(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    KalidousForPayload data = {};

    // Detect for-in: either bare or parenthesised
    //   for x in col { }
    //   for (x in col) { }
    bool paren_for_in = false;
    if (parser_check(p, KALIDOUS_TOKEN_LPAREN) &&
        parser_peek_ahead(p, 1)->type == KALIDOUS_TOKEN_IDENTIFIER &&
        parser_peek_ahead(p, 2)->type == KALIDOUS_TOKEN_IN) {
        parser_advance(p); // consume '('
        paren_for_in = true;
    }

    if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER) &&
        parser_peek_ahead(p, 1)->type == KALIDOUS_TOKEN_IN) {
        data.is_for_in    = true;
        data.iterator_var = parser_parse_expression(p);
        parser_advance(p); // consume 'in'
        data.iterable     = parser_parse_expression(p);
        if (paren_for_in)
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after for-in");
        data.body = parse_body(p);
        return kalidous_ast_make_for(p->arena, loc, data);
    }

    // TODO: for init; cond; step { }  — classic C-style
    data.is_for_in = false;
    data.condition = parser_parse_expression(p);
    data.body      = parse_body(p);
    return kalidous_ast_make_for(p->arena, loc, data);
}

static KalidousNode* parse_switch(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    KalidousNode* subject = parser_parse_expression(p);
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{' after switch expression");
    ArenaList<KalidousNode*> arms_b;
    arms_b.init(p->arena, 8);
    KalidousNode* default_arm = nullptr;
    // TODO: parse case pattern [if guard]: stmt*
    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p))
        parser_advance(p);
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");
    size_t         arm_count = 0;
    KalidousNode** arms      = arms_b.flatten(p->arena, &arm_count);
    const KalidousSwitchPayload data = { subject, arms, arm_count, default_arm };
    return kalidous_ast_make_switch(p->arena, loc, data);
}

static KalidousNode* parse_try_catch(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    KalidousNode* try_block = parser_parse_block(p);
    KalidousTryCatchPayload data = { try_block, nullptr, 0, nullptr };
    if (parser_match(p, KALIDOUS_TOKEN_CATCH)) {
        if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
            const KalidousToken* evar = parser_advance(p);
            data.catch_var     = evar->lexeme.data;
            data.catch_var_len = evar->lexeme.len;
        }
        // TODO: catch e: ErrorType, multiple catch, finally
        data.catch_block = parser_parse_block(p);
    }
    return kalidous_ast_make_try_catch(p->arena, loc, data);
}

static KalidousNode* parse_return(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    if (p->inside_fn && p->fn_kind == KALIDOUS_FN_NORETURN)
        parser_error(p, loc, "'return' not allowed in noreturn fn");
    KalidousNode* value = nullptr;
    if (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON) && !parser_is_at_end(p))
        value = parser_parse_expression(p);
    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after return");
    return kalidous_ast_make_return(p->arena, loc, value);
}

static KalidousNode* parse_yield(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    if (!p->inside_fn || p->fn_kind != KALIDOUS_FN_ASYNC)
        parser_error(p, loc, "'yield' only allowed in async fn");
    KalidousNode* value = nullptr;
    if (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON) && !parser_is_at_end(p))
        value = parser_parse_expression(p);
    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after yield");
    return kalidous_ast_make_yield(p->arena, loc, value);
}

KalidousNode* parser_parse_block(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{'");
    ArenaList<KalidousNode*> stmts_b;
    stmts_b.init(p->arena, 16);
    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p))
        stmts_b.push(p->arena, parser_parse_statement(p));
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");
    size_t         count = 0;
    KalidousNode** stmts = stmts_b.flatten(p->arena, &count);
    return kalidous_ast_make_block(p->arena, loc, stmts, count);
}

// Forward declaration — needed by parse_struct_decl before the full definition
static KalidousNode* parse_func_body(Parser* p, KalidousFnKind kind,
                                     KalidousSourceLoc loc, KalidousVisibility visibility);
static KalidousNode* parse_struct_decl(Parser* p, KalidousVisibility struct_vis);

static KalidousNode* parse_enum_decl(Parser* p, KalidousVisibility enum_vis);

static KalidousNode* parse_param(Parser* p);

KalidousNode* parser_parse_statement(Parser* p) {
    // Recover from previous parse failure before attempting a new statement
    if (p->panic) {
        parser_synchronize(p);
        p->panic = false;
        if (parser_is_at_end(p)) return nullptr;
    }

    const KalidousSourceLoc loc = parser_peek(p)->loc;

    switch (parser_peek(p)->type) {
        case KALIDOUS_TOKEN_LET:   parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_LET);
        case KALIDOUS_TOKEN_VAR:   parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_VAR);
        case KALIDOUS_TOKEN_AUTO:  parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_AUTO);
        case KALIDOUS_TOKEN_CONST: parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_CONST);
        case KALIDOUS_TOKEN_IF:     return parse_if(p);
        case KALIDOUS_TOKEN_FOR:    return parse_for(p);
        case KALIDOUS_TOKEN_SWITCH: return parse_switch(p);
        case KALIDOUS_TOKEN_TRY:    return parse_try_catch(p);
        case KALIDOUS_TOKEN_RETURN: return parse_return(p);
        case KALIDOUS_TOKEN_YIELD:  return parse_yield(p);

        // Nested declarations — fn, struct, enum can appear inside fn bodies
        // Useful for local helpers, local types, and closures
        case KALIDOUS_TOKEN_FN: {
            parser_advance(p);
            return parse_func_body(p, KALIDOUS_FN_NORMAL, loc, KALIDOUS_VIS_PRIVATE);
        }
        case KALIDOUS_TOKEN_ASYNC: {
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'async'");
            return parse_func_body(p, KALIDOUS_FN_ASYNC, loc, KALIDOUS_VIS_PRIVATE);
        }
        case KALIDOUS_TOKEN_STRUCT:
            return parse_struct_decl(p, KALIDOUS_VIS_PRIVATE);
        case KALIDOUS_TOKEN_ENUM:
            return parse_enum_decl(p, KALIDOUS_VIS_PRIVATE);

        // Anonymous scoped block: { ... }
        // Distinct from struct literal — { at statement position is always a block
        case KALIDOUS_TOKEN_LBRACE:
            return parser_parse_block(p);

        case KALIDOUS_TOKEN_AWAIT: {
            parser_advance(p);
            KalidousNode* expr = parser_parse_expression(p);
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after await");
            return kalidous_ast_make_await(p->arena, loc, expr);
        }
        case KALIDOUS_TOKEN_BREAK: {
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after break");
            return kalidous_ast_make_break(p->arena, loc, nullptr, 0);
            // TODO: labelled break
        }
        case KALIDOUS_TOKEN_CONTINUE: {
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after continue");
            return kalidous_ast_make_continue(p->arena, loc, nullptr, 0);
            // TODO: labelled continue
        }
        case KALIDOUS_TOKEN_GOTO: {
            parser_advance(p);
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_NORMAL)
                parser_error(p, loc, "'goto' not allowed in normal fn (use flowing fn or noreturn fn)");
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_ASYNC)
                parser_error(p, loc, "'goto' not allowed in async fn");

            // goto scene [name] — special jump to scene block
            if (parser_match(p, KALIDOUS_TOKEN_SCENE)) {
                // optional scene label: goto scene level1;
                const char* scene_label     = nullptr;
                size_t      scene_label_len = 0;
                if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
                    const KalidousToken* t = parser_advance(p);
                    scene_label     = t->lexeme.data;
                    scene_label_len = t->lexeme.len;
                }
                parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after goto scene");
                const KalidousGotoPayload data = {
                    scene_label, scene_label_len, nullptr, 0, true
                };
                return kalidous_ast_make_goto(p->arena, loc, data);
            }

            // goto label [(args)] ;
            const KalidousToken* label = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                                       "expected label after goto");
            // Optional arguments: goto sample(42, x);
            ArenaList<KalidousNode*> args_b;
            args_b.init(p->arena, 4);
            if (parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
                while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    args_b.push(p->arena, parser_parse_expression(p));
                    if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                }
                parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after goto arguments");
            }
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after goto");
            size_t         arg_count = 0;
            KalidousNode** args      = args_b.flatten(p->arena, &arg_count);
            const KalidousGotoPayload data = {
                label->lexeme.data, label->lexeme.len, args, arg_count, false
            };
            return kalidous_ast_make_goto(p->arena, loc, data);
        }

        case KALIDOUS_TOKEN_MARKER: {
            parser_advance(p);
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_NORMAL)
                parser_error(p, loc, "'marker' not allowed in normal fn");
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_ASYNC)
                parser_error(p, loc, "'marker' not allowed in async fn");

            const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                                      "expected marker name");
            ArenaList<KalidousNode*> params_b;
            params_b.init(p->arena, 4);
            if (parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
                while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    params_b.push(p->arena, parse_param(p));
                    if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                }
                parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after marker params");
            }
            size_t         param_count = 0;
            KalidousNode** params      = params_b.flatten(p->arena, &param_count);
            KalidousNode*  body        = parse_body(p);
            const KalidousMarkerPayload data = {
                name->lexeme.data, name->lexeme.len, params, param_count, body
            };
            return kalidous_ast_make_marker(p->arena, loc, data);
        }

        // entry [name(params)] body
        // Anonymous = no name; entry main(...) is the special named form.
        case KALIDOUS_TOKEN_ENTRY: {
            parser_advance(p);
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_NORMAL)
                parser_error(p, loc, "'entry' not allowed in normal fn");
            if (p->inside_fn && p->fn_kind == KALIDOUS_FN_ASYNC)
                parser_error(p, loc, "'entry' not allowed in async fn");

            const char* ename     = nullptr;
            size_t      ename_len = 0;
            if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
                const KalidousToken* t = parser_advance(p);
                ename     = t->lexeme.data;
                ename_len = t->lexeme.len;
            }
            ArenaList<KalidousNode*> params_b;
            params_b.init(p->arena, 4);
            if (ename && parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
                while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    params_b.push(p->arena, parse_param(p));
                    if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                }
                parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after entry params");
            }
            size_t         param_count = 0;
            KalidousNode** params      = params_b.flatten(p->arena, &param_count);
            KalidousNode*  body        = parse_body(p);
            const KalidousMarkerPayload data = {
                ename, ename_len, params, param_count, body
            };
            return kalidous_ast_make_entry(p->arena, loc, data);
        }
        case KALIDOUS_TOKEN_SPAWN: {
            parser_advance(p);
            if (parser_check(p, KALIDOUS_TOKEN_LBRACE)) {
                KalidousNode* body = parser_parse_block(p);
                return kalidous_ast_make_spawn(p->arena, loc, body, true);
            }
            KalidousNode* expr = parser_parse_expression(p);
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after spawn expression");
            return kalidous_ast_make_spawn(p->arena, loc, expr, false);
        }

        //case KALIDOUS_TOKEN_ENTRY:
        default: {
            // check_kw fallback for 'entry' tokenized as IDENTIFIER
            // (happens when KALIDOUS_TOKEN_ENTRY is not yet in kalidous.h)
            if (parser_peek(p)->type == KALIDOUS_TOKEN_ENTRY || check_kw(p, "entry")) {
                parser_advance(p);
                if (p->inside_fn && p->fn_kind == KALIDOUS_FN_NORMAL)
                    parser_error(p, loc, "'entry' not allowed in normal fn");
                if (p->inside_fn && p->fn_kind == KALIDOUS_FN_ASYNC)
                    parser_error(p, loc, "'entry' not allowed in async fn");

                const char* ename     = nullptr;
                size_t      ename_len = 0;
                if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
                    const KalidousToken* t = parser_advance(p);
                    ename     = t->lexeme.data;
                    ename_len = t->lexeme.len;
                }
                ArenaList<KalidousNode*> params_b;
                params_b.init(p->arena, 4);
                if (ename && parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
                    while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                        params_b.push(p->arena, parse_param(p));
                        if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                    }
                    parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after entry params");
                }
                size_t         param_count = 0;
                KalidousNode** params      = params_b.flatten(p->arena, &param_count);
                KalidousNode*  body        = parse_body(p);
                const KalidousMarkerPayload data = {
                    ename, ename_len, params, param_count, body
                };
                return kalidous_ast_make_entry(p->arena, loc, data);
            }

            // Expression statement / assignment
            KalidousNode* expr = parser_parse_expression(p);
            const KalidousTokenType op = parser_peek(p)->type;
            if (op == KALIDOUS_TOKEN_ASSIGNMENT   ||
                op == KALIDOUS_TOKEN_DECLARATION  ||
                op == KALIDOUS_TOKEN_PLUS_EQUAL   ||
                op == KALIDOUS_TOKEN_MINUS_EQUAL  ||
                op == KALIDOUS_TOKEN_MULTIPLY_EQUAL ||
                op == KALIDOUS_TOKEN_DIVIDE_EQUAL) {
                const KalidousSourceLoc assign_loc = parser_peek(p)->loc;
                parser_advance(p);
                KalidousNode* value = parser_parse_expression(p);
                expr = kalidous_ast_make_binary_op(p->arena, assign_loc, op, expr, value);
            }
            parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after expression statement");
            return expr;
        }
    }
}

// ============================================================================
// Top-level declarations
// ============================================================================

static KalidousVisibility resolve_visibility(const KalidousToken* t) {
    const char*  d = t->lexeme.data;
    const size_t l = t->lexeme.len;
    if (l == 6 && strncmp(d, "public",    6) == 0) return KALIDOUS_VIS_PUBLIC;
    if (l == 9 && strncmp(d, "protected", 9) == 0) return KALIDOUS_VIS_PROTECTED;
    return KALIDOUS_VIS_PRIVATE;
}

static bool is_visibility_group(const Parser* p) {
    return parser_check(p, KALIDOUS_TOKEN_MODIFIER) &&
           parser_peek_ahead(p, 1)->type == KALIDOUS_TOKEN_COLON;
}

static bool is_visibility_inline(const Parser* p) {
    return parser_check(p, KALIDOUS_TOKEN_MODIFIER) &&
           parser_peek_ahead(p, 1)->type != KALIDOUS_TOKEN_COLON;
}

static KalidousNode* parse_field(Parser* p, KalidousVisibility vis) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;

    KalidousOwnership own = KALIDOUS_OWN_DEFAULT;
    if      (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
    else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
    else if (parser_match(p, KALIDOUS_TOKEN_VIEW))   own = KALIDOUS_OWN_VIEW;
    else if (parser_match(p, KALIDOUS_TOKEN_LEND))   own = KALIDOUS_OWN_LEND;

    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected field name");
    // Type annotation is optional: let name = default  vs  name: Type
    KalidousNode* type_node = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_COLON))
        type_node = parser_parse_type(p);

    KalidousNode* default_value = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT))
        default_value = parser_parse_expression(p);

    parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' after field declaration");

    const KalidousFieldPayload field = {
        name->lexeme.data, name->lexeme.len,
        own, vis, type_node, default_value
    };
    return kalidous_ast_make_field(p->arena, loc, field);
}

static KalidousNode* parse_enum_variant(Parser* p) {
    const KalidousSourceLoc loc  = parser_peek(p)->loc;
    const KalidousToken*    name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                                 "expected variant name");
    KalidousNode* value = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT))
        value = parser_parse_expression(p);
    // TODO: payload variants for family: Red { r: u8, g: u8, b: u8 }
    const KalidousEnumVariantPayload data = {
        name->lexeme.data, name->lexeme.len, value
    };
    return kalidous_ast_make_enum_variant(p->arena, loc, data);
}

static KalidousNode* parse_param(Parser* p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    KalidousOwnership own        = KALIDOUS_OWN_DEFAULT;
    bool              is_mutable = false;
    if      (parser_match(p, KALIDOUS_TOKEN_UNIQUE)) own = KALIDOUS_OWN_UNIQUE;
    else if (parser_match(p, KALIDOUS_TOKEN_SHARED)) own = KALIDOUS_OWN_SHARED;
    else if (parser_match(p, KALIDOUS_TOKEN_VIEW))   own = KALIDOUS_OWN_VIEW;
    else if (parser_match(p, KALIDOUS_TOKEN_LEND))   { own = KALIDOUS_OWN_LEND; is_mutable = true; }
    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected parameter name");
    parser_expect(p, KALIDOUS_TOKEN_COLON, "expected ':' after parameter name");
    KalidousNode* type_node = parser_parse_type(p);
    KalidousNode* default_value = nullptr;
    if (parser_match(p, KALIDOUS_TOKEN_ASSIGNMENT))
        default_value = parser_parse_expression(p);
    const KalidousParamPayload param = {
        name->lexeme.data, name->lexeme.len,
        own, type_node, default_value, is_mutable
    };
    return kalidous_ast_make_param(p->arena, loc, param);
}



static KalidousNode* parse_struct_decl(Parser* p, KalidousVisibility struct_vis) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);

    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected struct name");
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{'");

    ArenaList<KalidousNode*> fields_b, methods_b;
    fields_b.init(p->arena, 8);
    methods_b.init(p->arena, 4);

    KalidousVisibility current_vis = KALIDOUS_VIS_PRIVATE;

    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p)) {

        // Group: public: / private: / protected:
        if (is_visibility_group(p)) {
            current_vis = resolve_visibility(parser_peek(p));
            parser_advance(p);
            parser_advance(p);
            continue;
        }

        // Inline: public fn foo() or public name: Type
        KalidousVisibility item_vis = current_vis;
        if (is_visibility_inline(p)) {
            item_vis = resolve_visibility(parser_peek(p));
            parser_advance(p);
        }

        // Inline method — all fn kinds supported
        if (parser_check(p, KALIDOUS_TOKEN_FN)) {
            const KalidousSourceLoc fn_loc = parser_peek(p)->loc;
            parser_advance(p);
            methods_b.push(p->arena, parse_func_body(p, KALIDOUS_FN_NORMAL,
                                                     fn_loc, item_vis));
            continue;
        }
        if (parser_check(p, KALIDOUS_TOKEN_ASYNC)) {
            const KalidousSourceLoc fn_loc = parser_peek(p)->loc;
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'async'");
            methods_b.push(p->arena, parse_func_body(p, KALIDOUS_FN_ASYNC,
                                                     fn_loc, item_vis));
            continue;
        }
        if (check_kw(p, "noreturn")) {
            const KalidousSourceLoc fn_loc = parser_peek(p)->loc;
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'noreturn'");
            methods_b.push(p->arena, parse_func_body(p, KALIDOUS_FN_NORETURN,
                                                     fn_loc, item_vis));
            continue;
        }
        if (check_kw(p, "flowing")) {
            const KalidousSourceLoc fn_loc = parser_peek(p)->loc;
            parser_advance(p);
            parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'flowing'");
            methods_b.push(p->arena, parse_func_body(p, KALIDOUS_FN_FLOWING,
                                                     fn_loc, item_vis));
            continue;
        }

        // Field with binding keyword: let name [: Type] [= default];
        if (parser_check(p, KALIDOUS_TOKEN_LET)  ||
            parser_check(p, KALIDOUS_TOKEN_VAR)  ||
            parser_check(p, KALIDOUS_TOKEN_CONST)) {
            parser_advance(p);
            fields_b.push(p->arena, parse_field(p, item_vis));
            continue;
        }

        // Field without keyword: name: Type [= default];
        if (parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
            fields_b.push(p->arena, parse_field(p, item_vis));
            continue;
        }

        // Ownership modifier before field name
        if (parser_check(p, KALIDOUS_TOKEN_UNIQUE) ||
            parser_check(p, KALIDOUS_TOKEN_SHARED) ||
            parser_check(p, KALIDOUS_TOKEN_VIEW)   ||
            parser_check(p, KALIDOUS_TOKEN_LEND)) {
            fields_b.push(p->arena, parse_field(p, item_vis));
            continue;
        }

        parser_error(p, parser_peek(p)->loc, "unexpected token in struct body");
        parser_advance(p);
    }
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");

    size_t         field_count  = 0;
    KalidousNode** fields       = fields_b.flatten(p->arena, &field_count);
    size_t         method_count = 0;
    KalidousNode** methods      = methods_b.flatten(p->arena, &method_count);

    const KalidousStructPayload decl = {
        name->lexeme.data, name->lexeme.len,
        fields, field_count, methods, method_count,
        struct_vis
    };
    return kalidous_ast_make_struct(p->arena, loc, decl);
}

static KalidousNode* parse_enum_decl(Parser* p, KalidousVisibility enum_vis) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    parser_advance(p);
    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected enum name");
    parser_expect(p, KALIDOUS_TOKEN_LBRACE, "expected '{'");

    ArenaList<KalidousNode*> variants_b;
    variants_b.init(p->arena, 8);
    while (!parser_check(p, KALIDOUS_TOKEN_RBRACE) && !parser_is_at_end(p)) {
        variants_b.push(p->arena, parse_enum_variant(p));
        if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
    }
    parser_expect(p, KALIDOUS_TOKEN_RBRACE, "expected '}'");

    size_t         variant_count = 0;
    KalidousNode** variants      = variants_b.flatten(p->arena, &variant_count);

    const KalidousEnumPayload decl = {
        name->lexeme.data, name->lexeme.len,
        variants, variant_count, enum_vis
    };
    return kalidous_ast_make_enum(p->arena, loc, decl);
}

// Full implementation of the forward-declared parse_func_body
static KalidousNode* parse_func_body(Parser* p, KalidousFnKind kind,
                                     KalidousSourceLoc loc, KalidousVisibility visibility) {
    const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                              "expected function name");
    parser_expect(p, KALIDOUS_TOKEN_LPAREN, "expected '(' after function name");

    ArenaList<KalidousNode*> params_b;
    params_b.init(p->arena, 8);
    while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
        params_b.push(p->arena, parse_param(p));
        if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
    }
    parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after parameters");

    // Return type: both -> and : accepted
    KalidousNode* return_type = nullptr;
    if (kind != KALIDOUS_FN_NORETURN) {
        if (parser_match(p, KALIDOUS_TOKEN_ARROW) ||
            parser_match(p, KALIDOUS_TOKEN_COLON))
            return_type = parser_parse_type(p);
    }

    const KalidousFnKind outer_kind   = p->fn_kind;
    const bool           outer_inside = p->inside_fn;
    p->fn_kind   = kind;
    p->inside_fn = true;

    KalidousNode* body = nullptr;
    if (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON))
        body = parse_body(p);
    else
        parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';' for forward declaration");

    p->fn_kind   = outer_kind;
    p->inside_fn = outer_inside;

    size_t         param_count = 0;
    KalidousNode** params      = params_b.flatten(p->arena, &param_count);

    const KalidousFuncPayload decl = {
        name->lexeme.data, name->lexeme.len,
        kind, params, param_count,
        return_type, body,
        visibility, false
    };
    return kalidous_ast_make_func_decl(p->arena, loc, decl);
}

KalidousNode* parser_parse_declaration(Parser* p) {
    // Recover from previous parse failure before attempting a new declaration
    if (p->panic) {
        parser_synchronize(p);
        p->panic = false;
        if (parser_is_at_end(p)) return nullptr;
    }

    KalidousVisibility vis = p->current_visibility;
    if (is_visibility_inline(p)) {
        vis = resolve_visibility(parser_peek(p));
        parser_advance(p);
    }
    if (is_visibility_group(p)) {
        p->current_visibility = resolve_visibility(parser_peek(p));
        parser_advance(p);
        parser_advance(p);
        return nullptr; // group modifier — no node, just updates context
    }

    const KalidousSourceLoc loc = parser_peek(p)->loc;
    const KalidousTokenType t   = parser_peek(p)->type;

    if (t == KALIDOUS_TOKEN_STRUCT) return parse_struct_decl(p, vis);
    if (t == KALIDOUS_TOKEN_ENUM)   return parse_enum_decl(p, vis);

    // TODO: COMPONENT, UNION, FAMILY, ENTITY, TRAIT, IMPLEMENT, MODULE
    // TODO: use / import

    if (t == KALIDOUS_TOKEN_CONST) { parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_CONST); }
    if (t == KALIDOUS_TOKEN_LET)   { parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_LET);   }
    if (t == KALIDOUS_TOKEN_VAR)   { parser_advance(p); return parse_var_decl(p, KALIDOUS_BINDING_VAR);   }

    if (t == KALIDOUS_TOKEN_FN) {
        parser_advance(p);
        return parse_func_body(p, KALIDOUS_FN_NORMAL, loc, vis);
    }
    if (t == KALIDOUS_TOKEN_ASYNC) {
        parser_advance(p);
        parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'async'");
        return parse_func_body(p, KALIDOUS_FN_ASYNC, loc, vis);
    }
    // noreturn — dedicated token if available, fallback to check_kw for older builds
    if (t == KALIDOUS_TOKEN_NORETURN || check_kw(p, "noreturn")) {
        parser_advance(p);
        parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'noreturn'");
        return parse_func_body(p, KALIDOUS_FN_NORETURN, loc, vis);
    }
    // flowing — dedicated token if available, fallback to check_kw
    if (t == KALIDOUS_TOKEN_FLOWING || check_kw(p, "flowing")) {
        parser_advance(p);
        parser_expect(p, KALIDOUS_TOKEN_FN, "expected 'fn' after 'flowing'");
        return parse_func_body(p, KALIDOUS_FN_FLOWING, loc, vis);
    }

    // Top-level marker — no fn kind restriction (sema validates call sites)
    if (t == KALIDOUS_TOKEN_MARKER) {
        parser_advance(p);
        const KalidousToken* name = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER,
                                                  "expected marker name");
        ArenaList<KalidousNode*> params_b;
        params_b.init(p->arena, 4);
        if (parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
            while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                params_b.push(p->arena, parse_param(p));
                if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
            }
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')' after marker params");
        }
        size_t         param_count = 0;
        KalidousNode** params      = params_b.flatten(p->arena, &param_count);
        KalidousNode*  body        = parse_body(p);
        const KalidousMarkerPayload data = {
            name->lexeme.data, name->lexeme.len, params, param_count, body
        };
        return kalidous_ast_make_marker(p->arena, loc, data);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "unexpected token '%.*s' at top level",
             (int)parser_peek(p)->lexeme.len, parser_peek(p)->lexeme.data);
    parser_error(p, parser_peek(p)->loc, buf);
    KalidousNode* expr = parser_parse_expression(p);
    parser_match(p, KALIDOUS_TOKEN_SEMICOLON);
    return expr;
}

// ============================================================================
// Entry point — implements kalidous_parse declared in kalidous.h
// ============================================================================

KalidousNode* kalidous_parse(KalidousArena* arena, KalidousTokenStream tokens) {
    // Source is not available here — use kalidous_parse_with_source for diagnostics
    Parser p;
    parser_init(&p, arena, nullptr, 0, nullptr, tokens);

    ArenaList<KalidousNode*> decls_b;
    decls_b.init(arena, 16);
    while (!parser_is_at_end(&p)) {
        KalidousNode* decl = parser_parse_declaration(&p);
        if (decl) decls_b.push(arena, decl);
    }

    // Fallback: print diagnostics without source context
    kalidous_diag_print_all(&p.diags, nullptr, 0, "<input>");

    size_t         count = 0;
    KalidousNode** decls = decls_b.flatten(arena, &count);
    return kalidous_ast_make_program(arena, decls, count);
}

// Extended entry point — preferred; provides source lines in error output
KalidousNode* kalidous_parse_with_source(KalidousArena* arena,
                                          const char* source, size_t source_len,
                                          const char* filename,
                                          KalidousTokenStream tokens) {
    Parser p;
    parser_init(&p, arena, source, source_len, filename, tokens);

    ArenaList<KalidousNode*> decls_b;
    decls_b.init(arena, 16);
    while (!parser_is_at_end(&p)) {
        KalidousNode* decl = parser_parse_declaration(&p);
        if (decl) decls_b.push(arena, decl);
    }

    kalidous_diag_print_all(&p.diags, source, source_len, filename);

    size_t         count = 0;
    KalidousNode** decls = decls_b.flatten(arena, &count);
    return kalidous_ast_make_program(arena, decls, count);
}