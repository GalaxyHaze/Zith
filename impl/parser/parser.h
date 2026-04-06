// impl/parser/parser.h — Parser interface for Kalidous
//
// Refactored to use centralized modules:
//   - diagnostics.hpp for error reporting
//   - parser_context.hpp for parser state
//   - types.hpp for enums
//
// The Parser struct and diagnostic types are now defined in
// parser_context.hpp — this file re-exports them for compatibility
// and declares sub-parser functions.
#pragma once

#include "parser_context.hpp"

// Re-export for compatibility — all types live in parser_context.hpp
// Old code that includes parser.h will continue to work.

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Sub-parsers — declared here, implemented in parser_decl.cpp, parser_expr.cpp
// ============================================================================

KalidousNode *parser_parse_declaration(Parser *p);

KalidousNode *parser_parse_statement(Parser *p);

KalidousNode *parser_parse_expression(Parser *p);

KalidousNode *parser_parse_type(Parser *p);

KalidousNode *parser_parse_block(Parser *p);

// ============================================================================
// Legacy aliases — removed in favor of centralized functions
// ============================================================================

// Old code may call these — redirect to parser_context.hpp versions
// These are now implemented in parser_utils.cpp via parser_emit_diag

static inline void parser_emit(Parser *p, KalidousSourceLoc loc,
                               KalidousDiagSeverity severity, const char *msg) {
    parser_emit_diag(p, loc, severity, msg);
}

#ifdef __cplusplus
}
#endif