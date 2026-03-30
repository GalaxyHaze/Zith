// impl/parser/parser_utils.cpp
#include "parser.h"
#include "../memory/utils.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ============================================================================
// Internal Helpers
// ============================================================================

// Helper: Finds the start and length of a specific line number (1-based)
static bool find_source_line(const char *source, size_t source_len,
                             size_t line_num, const char **out_start, size_t *out_len) {
    const char *p = source;
    const char *end = source + source_len;
    size_t cur = 1;

    // Move pointer to the start of the target line
    while (p < end && cur < line_num) {
        if (*p++ == '\n') cur++;
    }
    if (cur != line_num) return false;

    const char *line_start = p;
    // Find the end of the line
    while (p < end && *p != '\n') p++;

    *out_start = line_start;
    *out_len = (size_t)(p - line_start);
    return true;
}

static const char *severity_label(KalidousDiagSeverity s) {
    switch (s) {
        case KALIDOUS_DIAG_ERROR: return "error";
        case KALIDOUS_DIAG_WARNING: return "warning";
        case KALIDOUS_DIAG_NOTE: return "note";
        default: return "info";
    }
}

// ============================================================================
// Diagnostics
// ============================================================================

void kalidous_diag_print_all(const KalidousDiagList *diags, const char *source,
                             size_t source_len, const char *filename) {
    if (!diags || diags->count == 0) return;

    for (size_t i = 0; i < diags->count; ++i) {
        const KalidousDiagnostic *d = &diags->items[i];

        // 1. Print the Header: file:line:col: severity: message
        fprintf(stderr, "%s:%zu:%zu: %s: %s\n", 
                filename ? filename : "<input>",
                d->loc.line, d->loc.index, 
                severity_label(d->severity), d->message);

        // 2. Print the Source Line and Caret
        if (source && source_len > 0) {
            const char *line_ptr = nullptr;
            size_t line_len = 0;

            if (find_source_line(source, source_len, d->loc.line, &line_ptr, &line_len)) {
                // Print the actual code line
                fprintf(stderr, "  %.*s\n", (int)line_len, line_ptr);

                // Print the caret (^^^) pointing to the error
                fprintf(stderr, "  ");
                size_t col = d->loc.index;
                // Handle tabs roughly by assuming tab width 4 or just printing space
                for (size_t c = 0; c < col && c < line_len; ++c) {
                    fputc(line_ptr[c] == '\t' ? '\t' : ' ', stderr);
                }
                fprintf(stderr, "^\n");
            }
        }
    }

    // 3. Print Summary
    size_t errors = 0, warnings = 0;
    for (size_t i = 0; i < diags->count; ++i) {
        if (diags->items[i].severity == KALIDOUS_DIAG_ERROR) errors++;
        else if (diags->items[i].severity == KALIDOUS_DIAG_WARNING) warnings++;
    }

    if (errors > 0 || warnings > 0) {
        fprintf(stderr, "\n%s: ", filename ? filename : "<input>");
        if (errors) fprintf(stderr, "%zu error(s)", errors);
        if (errors && warnings) fprintf(stderr, ", ");
        if (warnings) fprintf(stderr, "%zu warning(s)", warnings);
        fprintf(stderr, "\n\n");
    }
}

// ============================================================================
// Token Navigation
// ============================================================================

const KalidousToken *parser_peek(const Parser *p) {
    if (p->pos < p->count) return &p->tokens[p->pos];
    static constexpr KalidousToken eof = {{nullptr, 0}, {0, 0}, KALIDOUS_TOKEN_END, 0};
    return &eof;
}

const KalidousToken *parser_peek_ahead(const Parser *p, size_t offset) {
    size_t idx = p->pos + offset;
    if (idx < p->count) return &p->tokens[idx];
    static constexpr KalidousToken eof = {{nullptr, 0}, {0, 0}, KALIDOUS_TOKEN_END, 0};
    return &eof;
}

const KalidousToken *parser_advance(Parser *p) {
    const KalidousToken *t = parser_peek(p);
    if (p->pos < p->count) p->pos++;
    return t;
}

bool parser_check(const Parser *p, KalidousTokenType type) {
    return parser_peek(p)->type == type;
}

bool parser_match(Parser *p, KalidousTokenType type) {
    if (parser_check(p, type)) { parser_advance(p); return true; }
    return false;
}

const KalidousToken *parser_expect(Parser *p, KalidousTokenType type, const char *msg) {
    if (parser_check(p, type)) return parser_advance(p);
    
    // CRITICAL FIX: Do not emit error if already in panic mode.
    // This prevents "cascading" errors (printing 50 errors because of one missing brace).
    if (p->panic) return parser_peek(p);
    
    const KalidousToken *t = parser_peek(p);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (got '%.*s')", msg, (int)t->lexeme.len, t->lexeme.data);
    parser_error(p, t->loc, buf);
    // Do NOT set p->panic = true here. 
    // The calling code or parser_error handles that. 
    // (Note: parser_error DOES set panic=true).
    return t;
}

bool parser_is_at_end(const Parser *p) { return parser_peek(p)->type == KALIDOUS_TOKEN_END; }

bool check_kw(const Parser *p, const char *kw) {
    const KalidousToken *t = parser_peek(p);
    if (t->type != KALIDOUS_TOKEN_IDENTIFIER) return false;
    size_t len = strlen(kw);
    return t->lexeme.len == len && memcmp(t->lexeme.data, kw, len) == 0;
}

// ============================================================================
// Error Handling & Synchronization
// ============================================================================

void parser_emit(Parser *p, const KalidousSourceLoc loc,
                        KalidousDiagSeverity severity, const char *msg) {
    if (p->diags.count >= p->diags.capacity) {
        size_t new_cap = p->diags.capacity == 0 ? 8 : p->diags.capacity * 2;
        auto *buf = static_cast<KalidousDiagnostic *>(
            kalidous_arena_alloc(p->arena, new_cap * sizeof(KalidousDiagnostic)));
        if (!buf) return;
        if (p->diags.items)
            memcpy(buf, p->diags.items, p->diags.count * sizeof(KalidousDiagnostic));
        p->diags.items = buf;
        p->diags.capacity = new_cap;
    }
    KalidousDiagnostic d;
    d.message = kalidous_arena_strdup(p->arena, msg);
    d.loc = loc;
    d.severity = severity;
    p->diags.items[p->diags.count++] = d;
    if (severity == KALIDOUS_DIAG_ERROR) p->had_error = true;
}

void parser_synchronize(Parser *p) {
    // Consume tokens until we hit a synchronization point (statement boundary)
    while (!parser_is_at_end(p)) {
        if (parser_check(p, KALIDOUS_TOKEN_SEMICOLON)) { 
            parser_advance(p); // consume ';'
            p->panic = false;
            return; 
        }
        
        // Stop if we hit a token that starts a new declaration/statement
        switch (parser_peek(p)->type) {
            case KALIDOUS_TOKEN_FN: 
            case KALIDOUS_TOKEN_STRUCT: 
            case KALIDOUS_TOKEN_ENUM:
            case KALIDOUS_TOKEN_IF: 
            case KALIDOUS_TOKEN_FOR: 
            case KALIDOUS_TOKEN_RETURN:
            case KALIDOUS_TOKEN_LBRACE: 
            case KALIDOUS_TOKEN_RBRACE:
                p->panic = false; 
                return;
            default: 
                parser_advance(p); 
                break;
        }
    }
}

void parser_error(Parser *p, const KalidousSourceLoc loc, const char *msg) {
    // Standard behavior: if we are already panicking, don't report new errors
    // to avoid flooding the user with cascading failures.
    if (p->panic) return;
    
    parser_emit(p, loc, KALIDOUS_DIAG_ERROR, msg);
    p->panic = true; // Activate panic mode
    parser_synchronize(p);
}

void skip_block(Parser *p) {
    // Used in SCAN mode to blindly skip a block { ... }
    if (!parser_match(p, KALIDOUS_TOKEN_LBRACE)) {
        // If no brace, skip until semicolon
        while (!parser_check(p, KALIDOUS_TOKEN_SEMICOLON) && !parser_is_at_end(p)) parser_advance(p);
        parser_expect(p, KALIDOUS_TOKEN_SEMICOLON, "expected ';'");
        return;
    }
    
    // If brace, skip until matching closing brace
    int depth = 1;
    while (!parser_is_at_end(p) && depth > 0) {
        const KalidousToken *t = parser_advance(p);
        if (t->type == KALIDOUS_TOKEN_LBRACE) depth++;
        else if (t->type == KALIDOUS_TOKEN_RBRACE) depth--;
    }
}

// ============================================================================
// Literals
// ============================================================================

KalidousLiteral parse_lit_number(const char *data, size_t len, KalidousTokenType type) {
    char buf[64];
    if (len >= sizeof(buf)) return {KALIDOUS_LIT_INT, {.i64 = 0}};
    memcpy(buf, data, len); buf[len] = '\0';
    KalidousLiteral lit = {};
    if (type == KALIDOUS_TOKEN_HEXADECIMAL) {
        lit.kind = KALIDOUS_LIT_UINT; lit.value.u64 = (uint64_t)strtoull(buf, nullptr, 16);
    } else if (type == KALIDOUS_TOKEN_BINARY) {
        lit.kind = KALIDOUS_LIT_UINT;
        uint64_t val = 0;
        for (size_t i = 2; i < len; ++i) if (data[i] == '0' || data[i] == '1') val = (val << 1) | (data[i] - '0');
        lit.value.u64 = val;
    } else if (type == KALIDOUS_TOKEN_OCTAL) {
        lit.kind = KALIDOUS_LIT_UINT; lit.value.u64 = (uint64_t)strtoull(buf + 2, nullptr, 8);
    } else {
        for (size_t i = 0; i < len; ++i) if (buf[i] == '.') {
            lit.kind = KALIDOUS_LIT_FLOAT; lit.value.f64 = strtod(buf, nullptr); return lit;
        }
        lit.kind = KALIDOUS_LIT_INT; lit.value.i64 = (int64_t)strtoll(buf, nullptr, 10);
    }
    return lit;
}