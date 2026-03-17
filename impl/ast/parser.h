// impl/parser/kalidous_parser.h — Parser interface for Kalidous
#pragma once

#include <Kalidous/kalidous.h>
#include "ast.h"
#include "../memory/utils.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Diagnostics
// ============================================================================

typedef enum KalidousDiagSeverity {
    KALIDOUS_DIAG_ERROR   = 0,
    KALIDOUS_DIAG_WARNING = 1,
    KALIDOUS_DIAG_NOTE    = 2,
} KalidousDiagSeverity;

typedef struct KalidousDiagnostic {
    const char*          message;   // interned in arena
    KalidousSourceLoc    loc;
    KalidousDiagSeverity severity;
} KalidousDiagnostic;

// List of diagnostics produced during parsing.
// Owned by the arena — lives as long as the parse arena.
typedef struct KalidousDiagList {
    KalidousDiagnostic* items;
    size_t              count;
    size_t              capacity;
} KalidousDiagList;

// Print all diagnostics with source context to stderr.
// source / source_len are the original file bytes (already in arena).
void kalidous_diag_print_all(const KalidousDiagList* diags,
                              const char* source, size_t source_len,
                              const char* filename);

// ============================================================================
// Parser state
// ============================================================================

typedef struct Parser {
    KalidousArena*       arena;
    const KalidousToken* tokens;
    size_t               count;
    size_t               pos;

    // Original source — needed to print the error line
    const char*          source;
    size_t               source_len;
    const char*          filename;

    // Accumulated diagnostics
    KalidousDiagList     diags;

    // had_error: true if any ERROR was emitted
    bool                 had_error;

    // panic: set when parser_expect fails — cleared after synchronizing at a
    // statement/declaration boundary. While in panic mode, parser_expect
    // suppresses further errors to avoid cascades.
    bool                 panic;

    // Current function context
    KalidousFnKind       fn_kind;
    bool                 inside_fn;

    // Active group visibility modifier (default: private)
    KalidousVisibility   current_visibility;
} Parser;

// ============================================================================
// Parser init
// ============================================================================

// source / source_len are the original file bytes.
// filename is used in diagnostic output — may be NULL.
void parser_init(Parser* p, KalidousArena* arena,
                 const char* source, size_t source_len,
                 const char* filename,
                 KalidousTokenStream tokens);

// ============================================================================
// Diagnostic emission
// ============================================================================

void parser_error  (Parser* p, KalidousSourceLoc loc, const char* msg);
void parser_warning(Parser* p, KalidousSourceLoc loc, const char* msg);
void parser_note   (Parser* p, KalidousSourceLoc loc, const char* msg);

// ============================================================================
// Token navigation
// ============================================================================

const KalidousToken* parser_peek      (const Parser* p);
const KalidousToken* parser_peek_ahead(const Parser* p, size_t offset);
const KalidousToken* parser_advance   (Parser* p);
bool                 parser_check     (const Parser* p, KalidousTokenType type);
bool                 parser_match     (Parser* p, KalidousTokenType type);
const KalidousToken* parser_expect    (Parser* p, KalidousTokenType type,
                                       const char* msg);
bool                 parser_is_at_end (const Parser* p);

// ============================================================================
// Sub-parsers
// ============================================================================

KalidousNode* parser_parse_declaration(Parser* p);
KalidousNode* parser_parse_statement  (Parser* p);
KalidousNode* parser_parse_expression (Parser* p);
KalidousNode* parser_parse_type       (Parser* p);
KalidousNode* parser_parse_block      (Parser* p);

#ifdef __cplusplus
}
#endif