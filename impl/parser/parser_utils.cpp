// impl/parser/parser_utils.cpp — Token navigation, error handling, synchronization
//
// Refactored to use centralized DiagManager from diagnostics.hpp.
// All fprintf/printf diagnostic calls are now routed through DiagManager.
#include "../memory/arena.hpp"
#include "parser.h"
#include "../diagnostics/diagnostics.hpp"
#include <cstring>
#include <cstdlib>

// ============================================================================
// Parser Init
// ============================================================================

void parser_init(Parser *p, ZithArena *arena,
                 const char *source, const size_t source_len,
                 const char *filename,
                 const ZithTokenStream tokens) {
    p->arena = arena;
    p->source = source;
    p->source_len = source_len;
    p->filename = filename ? filename : "<input>";
    p->tokens = tokens.data;
    p->count = tokens.len;
    p->pos = 0;
    p->had_error = false;
    p->panic = false;
    p->fn_kind = ZITH_FN_NORMAL;
    p->inside_fn = false;
    p->current_visibility = ZITH_VIS_PRIVATE;
    p->mode = ZITH_MODE_SCAN;
    p->diags = {nullptr, 0, 0};
    p->scan_root = nullptr;
    p->import_roots = nullptr;
    p->import_root_count = 0;
}

// ============================================================================
// Token Navigation
// ============================================================================

const ZithToken *parser_peek(const Parser *p) {
    if (p->pos < p->count) return &p->tokens[p->pos];
    static constexpr ZithToken eof = {{nullptr, 0}, {0, 0}, ZITH_TOKEN_END, 0};
    return &eof;
}

const ZithToken *parser_peek_ahead(const Parser *p, size_t offset) {
    size_t idx = p->pos + offset;
    if (idx < p->count) return &p->tokens[idx];
    static constexpr ZithToken eof = {{nullptr, 0}, {0, 0}, ZITH_TOKEN_END, 0};
    return &eof;
}

const ZithToken *parser_advance(Parser *p) {
    const ZithToken *t = parser_peek(p);
    if (p->pos < p->count) p->pos++;
    return t;
}

bool parser_check(const Parser *p, ZithTokenType type) {
    return parser_peek(p)->type == type;
}

bool parser_match(Parser *p, ZithTokenType type) {
    if (parser_check(p, type)) { parser_advance(p); return true; }
    return false;
}

const ZithToken *parser_expect(Parser *p, ZithTokenType type, const char *msg) {
    if (parser_check(p, type)) return parser_advance(p);

    // CRITICAL FIX: Do not emit error if already in panic mode.
    // This prevents "cascading" errors (printing 50 errors because of one missing brace).
    if (p->panic) return parser_peek(p);

    const ZithToken *t = parser_peek(p);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (got '%.*s')", msg, (int)t->lexeme.len, t->lexeme.data);
    parser_error(p, t->loc, buf);
    return t;
}

bool parser_is_at_end(const Parser *p) { return parser_peek(p)->type == ZITH_TOKEN_END; }

bool parser_check_kw(const Parser *p, const char *kw) {
    const ZithToken *t = parser_peek(p);
    if (t->type != ZITH_TOKEN_IDENTIFIER) return false;
    size_t len = strlen(kw);
    return t->lexeme.len == len && memcmp(t->lexeme.data, kw, len) == 0;
}

// Legacy alias for check_kw used in some files
bool check_kw(const Parser *p, const char *kw) { return parser_check_kw(p, kw); }

// ============================================================================
// Error Handling & Synchronization
// ============================================================================

void parser_emit_diag(Parser *p, ZithSourceLoc loc,
                      ZithDiagSeverity severity, const char *msg) {
    if (p->diags.count >= p->diags.capacity) {
        size_t new_cap = p->diags.capacity == 0 ? 8 : p->diags.capacity * 2;
        auto *buf = static_cast<ZithDiagnostic *>(
            zith_arena_alloc(p->arena, new_cap * sizeof(ZithDiagnostic)));
        if (!buf) return;
        if (p->diags.items)
            memcpy(buf, p->diags.items, p->diags.count * sizeof(ZithDiagnostic));
        p->diags.items = buf;
        p->diags.capacity = new_cap;
    }
    ZithDiagnostic d;
    d.message = zith_arena_strdup(p->arena, msg);
    d.loc = loc;
    d.severity = severity;
    p->diags.items[p->diags.count++] = d;
    if (severity == ZITH_DIAG_ERROR) p->had_error = true;
}

void parser_synchronize(Parser *p) {
    // Consume tokens until we hit a synchronization point (statement boundary)
    while (!parser_is_at_end(p)) {
        if (parser_check(p, ZITH_TOKEN_SEMICOLON)) {
            parser_advance(p); // consume ';'
            p->panic = false;
            return;
        }

        // Stop if we hit a token that starts a new declaration/statement
        switch (parser_peek(p)->type) {
            case ZITH_TOKEN_FN:
            case ZITH_TOKEN_STRUCT:
            case ZITH_TOKEN_ENUM:
            case ZITH_TOKEN_IF:
            case ZITH_TOKEN_FOR:
            case ZITH_TOKEN_RETURN:
            case ZITH_TOKEN_LBRACE:
            case ZITH_TOKEN_RBRACE:
                p->panic = false;
                return;
            default:
                parser_advance(p);
                break;
        }
    }
}

void parser_error(Parser *p, ZithSourceLoc loc, const char *msg) {
    // Standard behavior: if we are already panicking, don't report new errors
    // to avoid flooding the user with cascading failures.
    if (p->panic) return;

    parser_emit_diag(p, loc, ZITH_DIAG_ERROR, msg);
    p->panic = true; // Activate panic mode
    parser_synchronize(p);
}

void parser_warning(Parser *p, ZithSourceLoc loc, const char *msg) {
    parser_emit_diag(p, loc, ZITH_DIAG_WARNING, msg);
}

void parser_note(Parser *p, ZithSourceLoc loc, const char *msg) {
    parser_emit_diag(p, loc, ZITH_DIAG_NOTE, msg);
}

void skip_block(Parser *p) {
    // Used in SCAN mode to blindly skip a block { ... }
    if (!parser_match(p, ZITH_TOKEN_LBRACE)) {
        // If no brace, skip until semicolon
        while (!parser_check(p, ZITH_TOKEN_SEMICOLON) && !parser_is_at_end(p)) parser_advance(p);
        parser_expect(p, ZITH_TOKEN_SEMICOLON, "expected ';'");
        return;
    }

    // If brace, skip until matching closing brace
    int depth = 1;
    while (!parser_is_at_end(p) && depth > 0) {
        const ZithToken *t = parser_advance(p);
        if (t->type == ZITH_TOKEN_LBRACE) depth++;
        else if (t->type == ZITH_TOKEN_RBRACE) depth--;
    }
}

void parser_set_import_roots(Parser *p, const char **roots, size_t count) {
    p->import_roots = roots;
    p->import_root_count = count;
}

// ============================================================================
// Literals
// ============================================================================

ZithLiteral parse_lit_number(const char *data, size_t len, ZithTokenType type) {
    char buf[64];
    if (len >= sizeof(buf)) return {ZITH_LIT_INT, {.i64 = 0}};
    memcpy(buf, data, len); buf[len] = '\0';
    ZithLiteral lit = {};
    if (type == ZITH_TOKEN_HEXADECIMAL) {
        lit.kind = ZITH_LIT_UINT; lit.value.u64 = (uint64_t)strtoull(buf, nullptr, 16);
    } else if (type == ZITH_TOKEN_BINARY) {
        lit.kind = ZITH_LIT_UINT;
        uint64_t val = 0;
        for (size_t i = 2; i < len; ++i) if (data[i] == '0' || data[i] == '1') val = (val << 1) | (data[i] - '0');
        lit.value.u64 = val;
    } else if (type == ZITH_TOKEN_OCTAL) {
        lit.kind = ZITH_LIT_UINT; lit.value.u64 = (uint64_t)strtoull(buf + 2, nullptr, 8);
    } else {
        for (size_t i = 0; i < len; ++i) if (buf[i] == '.') {
            lit.kind = ZITH_LIT_FLOAT; lit.value.f64 = strtod(buf, nullptr); return lit;
        }
        lit.kind = ZITH_LIT_INT; lit.value.i64 = (int64_t)strtoll(buf, nullptr, 10);
    }
    return lit;
}