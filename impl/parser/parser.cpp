// impl/parser/parser.cpp — Parser entry point and pipeline orchestration
//
// Refactored to use centralized modules:
//   - parser_context.hpp for parser state
//   - diagnostics.hpp for error reporting
//   - arena.hpp for memory management
#include "../memory/arena.hpp"
#include "parser.h"
#include <cstdio>
#include <cstring>

// Use the kalidous::ArenaList template
using kalidous::ArenaList;

// ============================================================================
// Pipeline Orchestration
// ============================================================================

static KalidousNode *run_parser_phase(Parser *p, KalidousParserMode mode) {
    p->pos = 0;
    p->panic = false;
    p->had_error = false;
    p->mode = mode;

    ArenaList<KalidousNode *> decls_b; 
    decls_b.init(p->arena, 16);
    
    while (!parser_is_at_end(p)) {
        size_t pos_before = p->pos;
        KalidousNode *decl = parser_parse_declaration(p);
        if (decl) decls_b.push(p->arena, decl);
        // Anti-stall: if we didn't consume a token and aren't at end, force advance
        if (p->pos == pos_before && !parser_is_at_end(p)) parser_advance(p);
    }
    
    size_t count = 0;
    KalidousNode **decls = decls_b.flatten(p->arena, &count);
    return kalidous_ast_make_program(p->arena, decls, count);
}

KalidousNode *kalidous_parse_with_source(KalidousArena *arena, const char *source, size_t source_len,
                                         const char *filename, KalidousTokenStream tokens) {
    Parser p;
    parser_init(&p, arena, source, source_len, filename, tokens);

    // ─── Phase 1: SCAN ──────────────────────────────────────────────────
    // Parse declarations at signature level. Function bodies are captured
    // as UNBODY nodes (raw token streams) — the parser does NOT descend
    // into block contents. Structs, imports, and top-level expressions are
    // fully parsed.
    KalidousNode *scan_root = run_parser_phase(&p, KALIDOUS_MODE_SCAN);
    p.scan_root = scan_root;

    // ─── Phase 2: EXPAND ────────────────────────────────────────────────
    // Walk the tree and replace UNBODY nodes with fully parsed BLOCK nodes.
    // This is where statement-level parsing happens inside function bodies.
    // TODO: implement UNBODY → BLOCK expansion
    KalidousNode *expanded = scan_root; // pass-through for now

    // ─── Phase 3: SEMA ──────────────────────────────────────────────────
    // Semantic analysis: name resolution, type checking, borrow checker,
    // control-flow analysis. Produces an annotated AST.
    // TODO: implement semantic analysis
    KalidousNode *sema_result = expanded; // pass-through for now

    // Print diagnostics accumulated across all phases
    extern void kalidous_diag_print_all(const KalidousDiagList*, const char*, size_t, const char*);
    kalidous_diag_print_all(&p.diags, source, source_len, filename);

    return sema_result;
}

// ============================================================================
// Test API — global arena + RAII
// ============================================================================

// Shared arena for all test calls — lazily created, reset between calls.
static KalidousArena *g_test_arena = nullptr;

static KalidousArena *test_arena_or_init() {
    if (!g_test_arena) g_test_arena = kalidous_arena_create(1 << 20); // 1 MB
    else kalidous_arena_reset(g_test_arena);
    return g_test_arena;
}

KalidousNode *kalidous_parse_test(const char *source) {
    if (!source) return nullptr;

    const size_t len = strlen(source);
    KalidousArena *arena = test_arena_or_init();
    if (!arena) return nullptr;

    KalidousTokenStream tokens = kalidous_tokenize(arena, source, len);
    if (!tokens.data) return nullptr;

    Parser p;
    parser_init(&p, arena, source, len, "<test>", tokens);

    KalidousNode *root = run_parser_phase(&p, KALIDOUS_MODE_SCAN);

    kalidous_diag_print_all(&p.diags, source, len, "<test>");
    return root;
}

void kalidous_test_arena_destroy(void) {
    if (g_test_arena) { kalidous_arena_destroy(g_test_arena); g_test_arena = nullptr; }
}

// ParseResult C++ methods
#ifdef __cplusplus
void ParseResult::reset() {
    if (node) {
        // Arena is reset so it's clean for the next call.
        // We don't destroy it — it's reused across tests.
        if (g_test_arena) kalidous_arena_reset(g_test_arena);
        node = nullptr;
    }
}
#endif