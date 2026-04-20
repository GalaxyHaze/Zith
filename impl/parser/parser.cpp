// impl/parser/parser.cpp — Parser entry point and pipeline orchestration
//
// Refactored to use centralized modules:
//   - parser_context.hpp for parser state
//   - diagnostics.hpp for error reporting
//   - arena.hpp for memory management
#include "../memory/arena.hpp"
#include "../lexer/debug.h"
#include "parser.h"
#include <cstdio>
#include <cstring>

// Use the zith::ArenaList template
using zith::ArenaList;

// ============================================================================
// Pipeline Orchestration
// ============================================================================

static ZithNode *run_parser_phase(Parser *p, ZithParserMode mode) {
    p->pos = 0;
    p->panic = false;
    p->had_error = false;
    p->mode = mode;

    if (mode == ZITH_MODE_SCAN) {
        extern void clear_scanned_symbols();
        clear_scanned_symbols();
    }

    ArenaList<ZithNode *> decls_b; 
    decls_b.init(p->arena, 16);
    
    while (!parser_is_at_end(p)) {
        size_t pos_before = p->pos;
        ZithNode *decl = parser_parse_declaration(p);
        if (decl) decls_b.push(p->arena, decl);
        // Anti-stall: if we didn't consume a token and aren't at end, force advance
        if (p->pos == pos_before && !parser_is_at_end(p)) parser_advance(p);
    }
    
    size_t count = 0;
    ZithNode **decls = decls_b.flatten(p->arena, &count);
    return zith_ast_make_program(p->arena, decls, count);
}

ZithNode *zith_parse_with_source(ZithArena *arena, const char *source, size_t source_len,
                                         const char *filename, ZithTokenStream tokens) {
    Parser p;
    parser_init(&p, arena, source, source_len, filename, tokens);

    // ─── Phase 1: SCAN ──────────────────────────────────────────────────
    // Parse declarations at signature level. Function bodies are captured
    // as UNBODY nodes (raw token streams) — the parser does NOT descend
    // into block contents. Structs, imports, and top-level expressions are
    // fully parsed.
    ZithNode *scan_root = run_parser_phase(&p, ZITH_MODE_SCAN);
    p.scan_root = scan_root;

    // ─── Phase 2: EXPAND ────────────────────────────────────────────────
    // Walk the tree and replace UNBODY nodes with fully parsed BLOCK nodes.
    // This is where statement-level parsing happens inside function bodies.
    // TODO: implement UNBODY → BLOCK expansion
    ZithNode *expanded = scan_root; // pass-through for now

    // ─── Phase 3: SEMA ──────────────────────────────────────────────────
    // Semantic analysis: name resolution, type checking, borrow checker,
    // control-flow analysis. Produces an annotated AST.
    // TODO: implement semantic analysis
    ZithNode *sema_result = expanded; // pass-through for now

    // Print diagnostics accumulated across all phases
    extern void zith_diag_print_all(const ZithDiagList*, const char*, size_t, const char*);
    zith_diag_print_all(&p.diags, source, source_len, filename);

    return sema_result;
}

// ============================================================================
// Test API — global arena + RAII
// ============================================================================

// Shared arena for all test calls — lazily created, reset between calls.
static ZithArena *g_test_arena = nullptr;

static ZithArena *test_arena_or_init() {
    if (!g_test_arena) g_test_arena = zith_arena_create(1 << 20); // 1 MB
    else zith_arena_reset(g_test_arena);
    return g_test_arena;
}

ZithNode *zith_parse_test(const char *source) {
    if (!source) return nullptr;

    const size_t len = strlen(source);
    ZithArena *arena = test_arena_or_init();
    if (!arena) return nullptr;

    ZithTokenStream tokens = zith_tokenize(arena, source, len);
    if (!tokens.data) return nullptr;

    zith_debug_tokens(tokens.data, tokens.len);

    Parser p;
    parser_init(&p, arena, source, len, "<test>", tokens);

    ZithNode *root = run_parser_phase(&p, ZITH_MODE_SCAN);

    zith_ast_print(root, 0);
    zith_diag_print_all(&p.diags, source, len, "<test>");
    return root;
}

void zith_test_arena_destroy(void) {
    if (g_test_arena) { zith_arena_destroy(g_test_arena); g_test_arena = nullptr; }
}

// ParseResult C++ methods
#ifdef __cplusplus
void ParseResult::reset() {
    if (node) {
        // Arena is reset so it's clean for the next call.
        // We don't destroy it — it's reused across tests.
        if (g_test_arena) zith_arena_reset(g_test_arena);
        node = nullptr;
    }
}
#endif