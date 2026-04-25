// impl/parser/parser.h — Parser interface for Zith
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

ZithNode *parser_parse_declaration(Parser *p);

ZithNode *parser_parse_statement(Parser *p);

ZithNode *parser_parse_expression(Parser *p);

ZithNode *parser_parse_type(Parser *p);

ZithNode *parser_parse_block(Parser *p);

// ============================================================================
// Convenience API for tests
// ============================================================================

// Parse a source string using a shared global arena.
// The arena is reset on each call — the previous result becomes invalid.
// For C++ users, prefer the RAII wrapper `ParseResult` below.
// Diagnostics are printed to stderr; returns nullptr on lex error.
ZithNode *zith_parse_test(const char *source);

// Full pipeline helper for tests (SCAN + EXPAND + SEMA).
// Returns nullptr when semantic errors are produced.
ZithNode *zith_parse_test_full(const char *source);

// Cleanup the global test arena (call once at end of test suite).
void zith_test_arena_destroy(void);

#ifdef __cplusplus
} // extern "C"

// ============================================================================
// C++ RAII test wrapper — auto-resets the global arena on destruction
// ============================================================================

struct ParseResult {
    ZithNode *node = nullptr;

    ParseResult() = default;
    explicit ParseResult(ZithNode *n) : node(n) {}

    ~ParseResult() { reset(); }

    // non-copyable — the arena is shared, copies would dangle
    ParseResult(const ParseResult &) = delete;
    ParseResult &operator=(const ParseResult &) = delete;

    // movable — transfers ownership
    ParseResult(ParseResult &&o) noexcept : node(o.node) { o.node = nullptr; }
    ParseResult &operator=(ParseResult &&o) noexcept {
        if (this != &o) { reset(); node = o.node; o.node = nullptr; }
        return *this;
    }

    // Access
    ZithNode *get() const { return node; }
    ZithNode *operator->() const { return node; }
    explicit operator bool() const { return node != nullptr; }

    // Reset the global arena, invalidating this result.
    // Safe to call multiple times.
    void reset();
};

// C++ convenience — returns RAII wrapper.
inline ParseResult parse_test(const char *source) {
    return ParseResult(zith_parse_test(source));
}
#endif // __cplusplus

// ============================================================================
// Legacy alias — available in both C and C++
// ============================================================================

static inline void parser_emit(Parser *p, ZithSourceLoc loc,
                               ZithDiagSeverity severity, const char *msg) {
    parser_emit_diag(p, loc, severity, msg);
}
