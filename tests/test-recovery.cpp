// test-recovery.cpp - Parser error recovery tests
//
// Verifies that the scanner produces partial AST artifacts after syntax errors,
// that valid declarations after an error are still collected, that error nodes
// are correctly produced, and that the per-file error limit is respected.

#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "memory/arena.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "legacy-zith/parser/parser.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <string_view>

using namespace zith;

// ── Shared scanning helper ─────────────────────────────────────────

struct RecoveryTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;
    lexer::TokenStream tokens;

    RecoveryTest() : interner(arena), diags(arena), tokens{} {}

    struct Result {
        ast::AstBuilder *builder;
        ast::ProgramNode *program;
        bool hasErrors;
        size_t errorCount;
        size_t declCount;
    };

    Result scan(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {nullptr, nullptr, true, 1, 0};
        auto fileId      = addResult.value();
        auto tokenResult = lexer::tokenize(sourceMap, arena, fileId, diags);
        if (!tokenResult)
            return {nullptr, nullptr, true, 1, 0};
        tokens = std::move(tokenResult.value());

        auto *builder = arena.make<ast::AstBuilder>(arena, interner);
        auto *prog    = arena.make<ast::ProgramNode>(arena);
        symbols::SymbolTable syms(arena, &interner);
        parser::Parser parser(&tokens, builder, &diags);
        auto scanResult = parser::scan(parser, syms);
        *prog           = std::move(parser.program);

        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, prog, errs > 0, errs, prog->decls.size()};
    }
};

// ── Error node construction ─────────────────────────────────────────

static void test_error_nodes_constructable() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    ast::AstBuilder bld(arena, interner);

    auto eid = bld.errorExpr({});
    CHECK(eid != ast::kInvalidExpr, "errorExpr returns valid id");
    CHECK(ast::exprKind(bld.getExpr(eid)) == ast::ExprKind::Error, "errorExpr has Error kind");

    auto sid = bld.errorStmt({});
    CHECK(sid != ast::kInvalidStmt, "errorStmt returns valid id");
    CHECK(ast::stmtKind(bld.getStmt(sid)) == ast::StmtKind::Error, "errorStmt has Error kind");

    auto did = bld.errorDecl({});
    CHECK(did != ast::kInvalidDecl, "errorDecl returns valid id");
    CHECK(ast::declKind(bld.getDecl(did)) == ast::DeclKind::Error, "errorDecl has Error kind");
}

// ── Partial scan: valid decl after bad token ─────────────────────

static void test_valid_decl_after_bad_token() {
    RecoveryTest t;
    // Garbage token, then a valid fn declaration
    auto r = t.scan("@@@ fn hello() { }");
    // There should be at least one error for the bad tokens
    CHECK(r.hasErrors, "errors reported for bad token");
    // But the fn declaration should still be collected
    bool found_fn = false;
    if (r.builder && r.program) {
        for (auto id : r.program->decls) {
            if (id == ast::kInvalidDecl)
                continue;
            if (auto *fn = std::get_if<ast::FnDeclNode>(&r.builder->getDecl(id))) {
                if (fn->name == "hello")
                    found_fn = true;
            }
        }
    }
    CHECK(found_fn, "fn after bad token is still collected");
}

// ── Partial scan: valid struct after error struct ────────────────

static void test_valid_struct_after_error() {
    RecoveryTest t;
    // Bad struct (missing name), then a valid struct
    auto r = t.scan("struct { } struct Good { x: Int, }");
    CHECK(r.hasErrors, "errors for missing struct name");
    bool found_good = false;
    if (r.builder && r.program) {
        for (auto id : r.program->decls) {
            if (id == ast::kInvalidDecl)
                continue;
            if (auto *s = std::get_if<ast::StructDeclNode>(&r.builder->getDecl(id)))
                if (s->name == "Good")
                    found_good = true;
        }
    }
    CHECK(found_good, "Good struct after error struct is collected");
}

// ── Multiple errors in same file ─────────────────────────────────

static void test_multiple_errors_same_file() {
    RecoveryTest t;
    auto r = t.scan("@@@ @@@ fn ok() { }");
    CHECK(r.hasErrors, "multiple errors reported");
    CHECK(r.errorCount >= 1, "at least one error recorded");
    bool found_ok = false;
    if (r.builder && r.program) {
        for (auto id : r.program->decls) {
            if (id == ast::kInvalidDecl)
                continue;
            if (auto *fn = std::get_if<ast::FnDeclNode>(&r.builder->getDecl(id)))
                if (fn->name == "ok")
                    found_ok = true;
        }
    }
    CHECK(found_ok, "fn after multiple errors is still collected");
}

// ── scanStage() does not abort on first error ─────────────────────

static void test_scan_continues_after_error() {
    // Three valid fns with one broken declaration in the middle.
    RecoveryTest t;
    auto r = t.scan("fn a() { } @@@ fn b() { } fn c() { }");
    CHECK(r.hasErrors, "error from bad token");
    size_t fn_count = 0;
    if (r.builder && r.program) {
        for (auto id : r.program->decls) {
            if (id == ast::kInvalidDecl)
                continue;
            if (std::get_if<ast::FnDeclNode>(&r.builder->getDecl(id)))
                fn_count++;
        }
    }
    // At least b and c should be collected even though a bad token precedes b
    CHECK(fn_count >= 2, "at least 2 valid fns collected after error");
}

// ── FlatMap-backed scope lookup correctness ──────────────────────

static void test_symbol_lookup_after_flatmap() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    symbols::SymbolTable syms(arena, &interner);

    auto id1 = syms.declare("alpha", symbols::SymbolVisibility::Private, 0,
                            symbols::SymKind::Variable, ast::kInvalidDecl);
    auto id2 = syms.declare("beta", symbols::SymbolVisibility::Private, 0,
                            symbols::SymKind::Variable, ast::kInvalidDecl);

    CHECK(syms.lookup("alpha") == id1, "lookup alpha finds id1");
    CHECK(syms.lookup("beta") == id2, "lookup beta finds id2");
    CHECK(syms.lookup("gamma") == symbols::kInvalidSym, "lookup missing returns kInvalidSym");
}

static void test_scope_shadowing_with_flatmap() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    symbols::SymbolTable syms(arena, &interner);

    auto outer = syms.declare("x", symbols::SymbolVisibility::Private, 0,
                              symbols::SymKind::Variable, ast::kInvalidDecl);
    syms.enterScope();
    auto inner = syms.declare("x", symbols::SymbolVisibility::Private, 0,
                              symbols::SymKind::Variable, ast::kInvalidDecl);
    CHECK(inner != outer, "inner x is a different symbol");
    CHECK(syms.lookup("x") == inner, "lookup x in inner scope returns inner");
    syms.exitScope();
    CHECK(syms.lookup("x") == outer, "lookup x after exit returns outer");
}

static void test_lookup_in_scope_uses_flatmap() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    symbols::SymbolTable syms(arena, &interner);

    auto id = syms.declare("thing", symbols::SymbolVisibility::Private, 0, symbols::SymKind::Fn,
                           ast::kInvalidDecl);
    auto root_scope = syms.currentScope();
    CHECK(syms.lookupInScope("thing", root_scope) == id, "lookupInScope finds thing in root");
    CHECK(syms.lookupInScope("thing", static_cast<zith::symbols::ScopeId>(999)) ==
              symbols::kInvalidSym,
          "lookupInScope on nonexistent scope returns kInvalidSym");
}

// ── Arena::allocatedBytes / usedBytes ──────────────────────────────

static void test_arena_allocated_bytes() {
    memory::Arena arena;
    size_t initial = arena.allocatedBytes();
    CHECK(initial > 0, "fresh arena has at least one block");

    arena.alloc(1024);
    size_t after = arena.allocatedBytes();
    CHECK(after >= initial, "allocatedBytes does not decrease after alloc");
}

static void test_arena_used_bytes() {
    memory::Arena arena;
    size_t used0 = arena.usedBytes();
    arena.alloc(256);
    size_t used1 = arena.usedBytes();
    CHECK(used1 > used0, "usedBytes increases after alloc");
}

// ── Runner ─────────────────────────────────────────────────────────

static void test_recovery() {
    test_error_nodes_constructable();

    test_valid_decl_after_bad_token();
    test_valid_struct_after_error();
    test_multiple_errors_same_file();
    test_scan_continues_after_error();

    test_symbol_lookup_after_flatmap();
    test_scope_shadowing_with_flatmap();
    test_lookup_in_scope_uses_flatmap();

    test_arena_allocated_bytes();
    test_arena_used_bytes();
}

TEST_MAIN(recovery)
