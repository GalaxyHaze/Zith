// impl/parser/parser_context.hpp — Parser state and token navigation
//
// Centralizes parser state management, token navigation, and synchronization.
// Extracted from parser.h/parser_utils.cpp to reduce coupling.
#pragma once

#include <zith/zith.hpp>
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

typedef struct ZithSymbolTable ZithSymbolTable;

typedef struct ZithScope {
    ZithSymbolTable *parent;
    ZithSymbolTable *symbols;
} ZithScope;

// ============================================================================
// Parser state
// ============================================================================

typedef struct Parser {
    ZithArena *arena;
    const ZithToken *tokens;
    size_t count;
    size_t pos;

    // Original source — needed to print the error line
    const char *source;
    size_t source_len;
    const char *filename;
    ZithSymbolTable *currentScope;

    // Accumulated diagnostics
    ZithDiagList diags;

    // had_error: true if any ERROR was emitted
    bool had_error;

    // panic: set when parser_expect fails — cleared after synchronizing
    bool panic;

    // Current function context
    ZithFnKind fn_kind;
    bool inside_fn;

    // Active group visibility modifier (default: private)
    ZithVisibility current_visibility;
    ZithParserMode mode;
    ZithNode *scan_root;

    // Import system - allowed import roots (std, utils, c, etc.)
    const char **import_roots;
    size_t import_root_count;
} Parser;

// ============================================================================
// Parser init
// ============================================================================

void parser_init(Parser *p, ZithArena *arena,
                 const char *source, size_t source_len,
                 const char *filename,
                 ZithTokenStream tokens);

void parser_set_import_roots(Parser *p, const char **roots, size_t count);
void parser_set_imported_decls(void *decls, ZithArena *arena);

// ============================================================================
// Token navigation
// ============================================================================

const ZithToken *parser_peek(const Parser *p);

const ZithToken *parser_peek_ahead(const Parser *p, size_t offset);

const ZithToken *parser_advance(Parser *p);

bool parser_check(const Parser *p, ZithTokenType type);

bool parser_match(Parser *p, ZithTokenType type);

const ZithToken *parser_expect(Parser *p, ZithTokenType type,
                                   const char *msg);

bool parser_is_at_end(const Parser *p);

// Check if current token is a specific keyword (by string)
bool parser_check_kw(const Parser *p, const char *kw);

// ============================================================================
// Error handling & synchronization
// ============================================================================

// Emit a diagnostic and potentially enter panic mode
void parser_emit_diag(Parser *p, ZithSourceLoc loc,
                      ZithDiagSeverity severity, const char *msg);

// Enter panic mode and synchronize to the next statement boundary
void parser_synchronize(Parser *p);

// Emit an error — sets panic mode
void parser_error(Parser *p, ZithSourceLoc loc, const char *msg);

// Emit a warning — does NOT set panic mode
void parser_warning(Parser *p, ZithSourceLoc loc, const char *msg);

// Emit a note — does NOT set panic mode
void parser_note(Parser *p, ZithSourceLoc loc, const char *msg);

// ============================================================================
// Scanner mode helpers
// ============================================================================

// Skip a block { ... } without parsing its contents — used in SCAN mode
void skip_block(Parser *p);

// NOTE: capture_unbody is an internal helper in parser_decl.cpp — not declared here
// to avoid linkage conflicts. It is used only within that translation unit.

#ifdef __cplusplus
} // extern "C"

// Print scanned symbols for debugging (declared in parser_decl.cpp)
void print_scanned_symbols();

// ============================================================================
// C++ ParserContext — wraps Parser with DiagManager
// ============================================================================

class ParserContext {
public:
    ParserContext() : parser_{} {}

    // Initialize the parser with source and tokens
    void init(ZITH::Arena &arena, const char *source, size_t source_len,
              const char *filename, ZithTokenStream tokens) {
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
    ZITH::Arena *arena() { return arena_; }
    const ZITH::Arena *arena() const { return arena_; }

private:
    Parser parser_;
    DiagManager diag_;
    ZITH::Arena *arena_ = nullptr;
};

#endif // __cplusplus
