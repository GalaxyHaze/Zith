// impl/parser/parser_context.hpp — Parser state and token navigation
//
// Centralizes parser state management, token navigation, and synchronization.
// Extracted from parser.h/parser_utils.cpp to reduce coupling.
#pragma once

#include <kalidous/kalidous.hpp>
#include "../ast/ast.h"
#include "../diagnostics/diagnostics.hpp"
#include "../memory/arena.hpp"
#include "../types/types.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Symbol table (forward declaration — full implementation later)
// ============================================================================

typedef struct KalidousSymbolTable KalidousSymbolTable;

typedef struct KalidousScope {
    KalidousSymbolTable *parent;
    KalidousSymbolTable *symbols;
} KalidousScope;

// ============================================================================
// Parser state
// ============================================================================

typedef struct Parser {
    KalidousArena *arena;
    const KalidousToken *tokens;
    size_t count;
    size_t pos;

    // Original source — needed to print the error line
    const char *source;
    size_t source_len;
    const char *filename;
    KalidousSymbolTable *currentScope;

    // Accumulated diagnostics
    KalidousDiagList diags;

    // had_error: true if any ERROR was emitted
    bool had_error;

    // panic: set when parser_expect fails — cleared after synchronizing
    bool panic;

    // Current function context
    KalidousFnKind fn_kind;
    bool inside_fn;

    // Active group visibility modifier (default: private)
    KalidousVisibility current_visibility;
    KalidousParserMode mode;
    KalidousNode *scan_root;
} Parser;

// ============================================================================
// Parser init
// ============================================================================

void parser_init(Parser *p, KalidousArena *arena,
                 const char *source, size_t source_len,
                 const char *filename,
                 KalidousTokenStream tokens);

// ============================================================================
// Token navigation
// ============================================================================

const KalidousToken *parser_peek(const Parser *p);

const KalidousToken *parser_peek_ahead(const Parser *p, size_t offset);

const KalidousToken *parser_advance(Parser *p);

bool parser_check(const Parser *p, KalidousTokenType type);

bool parser_match(Parser *p, KalidousTokenType type);

const KalidousToken *parser_expect(Parser *p, KalidousTokenType type,
                                   const char *msg);

bool parser_is_at_end(const Parser *p);

// Check if current token is a specific keyword (by string)
bool parser_check_kw(const Parser *p, const char *kw);

// ============================================================================
// Error handling & synchronization
// ============================================================================

// Emit a diagnostic and potentially enter panic mode
void parser_emit_diag(Parser *p, KalidousSourceLoc loc,
                      KalidousDiagSeverity severity, const char *msg);

// Enter panic mode and synchronize to the next statement boundary
void parser_synchronize(Parser *p);

// Emit an error — sets panic mode
void parser_error(Parser *p, KalidousSourceLoc loc, const char *msg);

// Emit a warning — does NOT set panic mode
void parser_warning(Parser *p, KalidousSourceLoc loc, const char *msg);

// Emit a note — does NOT set panic mode
void parser_note(Parser *p, KalidousSourceLoc loc, const char *msg);

// ============================================================================
// Scanner mode helpers
// ============================================================================

// Skip a block { ... } without parsing its contents — used in SCAN mode
void skip_block(Parser *p);

// NOTE: capture_unbody is an internal helper in parser_decl.cpp — not declared here
// to avoid linkage conflicts. It is used only within that translation unit.

#ifdef __cplusplus
} // extern "C"

// ============================================================================
// C++ ParserContext — wraps Parser with DiagManager
// ============================================================================

class ParserContext {
public:
    ParserContext() : parser_{} {}

    // Initialize the parser with source and tokens
    void init(KALIDOUS::Arena &arena, const char *source, size_t source_len,
              const char *filename, KalidousTokenStream tokens) {
        arena_ = &arena;
        diag_.set_arena(arena.get());
        parser_init(&parser_, arena.get(), source, source_len, filename, tokens);
    }

    // Access to raw parser state
    Parser &parser() { return parser_; }
    const Parser &parser() const { return parser_; }

    // Diagnostic access
    DiagManager &diag() { return diag_; }
    const DiagManager &diag() const { return diag_; }

    // Arena access
    KALIDOUS::Arena *arena() { return arena_; }
    const KALIDOUS::Arena *arena() const { return arena_; }

private:
    Parser parser_;
    DiagManager diag_;
    KALIDOUS::Arena *arena_ = nullptr;
};

#endif // __cplusplus
