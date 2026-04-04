// impl/parser/parser.cpp
#include "parser.h"
#include "../memory/utils.h"
#include <cstdio>
#include <cstring>

// Forward declarations of functions in other files
// In a real project, these would be in a header like "parser_internals.h"
extern KalidousNode *parser_parse_declaration(Parser *p);
extern void skip_block(Parser *p);

// ============================================================================
// Parser Init
// ============================================================================

void parser_init(Parser *p, KalidousArena *arena,
                 const char *source, const size_t source_len,
                 const char *filename,
                 const KalidousTokenStream tokens) {
    p->arena = arena;
    p->source = source;
    p->source_len = source_len;
    p->filename = filename ? filename : "<input>";
    p->tokens = tokens.data;
    p->count = tokens.len;
    p->pos = 0;
    p->had_error = false;
    p->panic = false;
    p->fn_kind = KALIDOUS_FN_NORMAL;
    p->inside_fn = false;
    p->current_visibility = KALIDOUS_VIS_PRIVATE;
    p->mode = KALIDOUS_MODE_PARSE;
    p->diags = {nullptr, 0, 0};
    p->scan_root = nullptr; 
}

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

    // 1. SCAN Mode
    // Parses signatures (structs, fns), captures bodies as UNBODY nodes.
    // Stores result in p.scan_root for use by later phases.
    KalidousNode *scan_root = run_parser_phase(&p, KALIDOUS_MODE_SCAN);
    p.scan_root = scan_root; 

    // 2. EXPAND Mode (Placeholder)
    // Hook for macro expansion or symbol table population based on scan_root.
    // Currently disabled/empty - will be implemented later.
    // For now, just pass through the scan_root
    KalidousNode *expand_root = scan_root;

    // 3. PARSE Mode
    // Uses the tree from EXPAND phase and parses UNBODY nodes with type checking.
    // This phase should navigate the existing tree, not create a new one from scratch.
    KalidousNode *final_ast = expand_root;
    
    // TODO: Implement UNBODY parsing in PARSE mode
    // The parser should traverse 'final_ast' and replace UNBODY nodes
    // with fully parsed BLOCK nodes, performing type checks like "does this type exist?"

    // Print diagnostics accumulated across all phases
    extern void kalidous_diag_print_all(const KalidousDiagList*, const char*, size_t, const char*);
    kalidous_diag_print_all(&p.diags, source, source_len, filename);
    
    return final_ast;
}