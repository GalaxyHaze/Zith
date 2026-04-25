// impl/parser/parser_test.cpp — Test utilities and RAII wrappers
//
// Refactored from parser.cpp. Provides convenience API for tests.
#include "../memory/arena.hpp"
#include "parser.h"
#include <cstring>

static ZithArena *g_test_arena = nullptr;

static ZithArena *test_arena_or_init() {
    if (!g_test_arena) g_test_arena = zith_arena_create(1 << 20);
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

    Parser p;
    parser_init(&p, arena, source, len, "<test>", tokens);

    p.pos = 0;
    p.panic = false;
    p.had_error = false;
    p.mode = ZITH_MODE_SCAN;

    extern void clear_scanned_symbols();
    clear_scanned_symbols();

    zith::ArenaList<ZithNode *> decls_b;
    decls_b.init(p.arena, 16);

    while (!parser_is_at_end(&p)) {
        size_t pos_before = p.pos;
        ZithNode *decl = parser_parse_declaration(&p);
        if (decl) decls_b.push(p.arena, decl);
        if (p.pos == pos_before && !parser_is_at_end(&p)) parser_advance(&p);
    }

    size_t count = 0;
    ZithNode **decls = decls_b.flatten(p.arena, &count);
    return zith_ast_make_program(p.arena, decls, count);
}

ZithNode *zith_parse_test_full(const char *source) {
    if (!source) return nullptr;

    const size_t len = strlen(source);
    ZithArena *arena = test_arena_or_init();
    if (!arena) return nullptr;

    ZithTokenStream tokens = zith_tokenize(arena, source, len);
    if (!tokens.data) return nullptr;

    return zith_parse_with_source(arena, source, len, "<test>", tokens);
}

void zith_test_arena_destroy(void) {
    if (g_test_arena) { zith_arena_destroy(g_test_arena); g_test_arena = nullptr; }
}

#ifdef __cplusplus
void ParseResult::reset() {
    if (node) {
        if (g_test_arena) zith_arena_reset(g_test_arena);
        node = nullptr;
    }
}
#endif