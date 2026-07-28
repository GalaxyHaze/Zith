#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/type-expr.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/parser.hpp"
#include "memory/arena.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <string_view>

using namespace zith;

struct AliasTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;

    AliasTest() : interner(arena), diags(arena) {}

    struct Result {
        ast::AstBuilder *builder;
        ast::ProgramNode *program;
        bool ok;
        size_t errorCount;
    };

    Result scan(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {nullptr, nullptr, false, 0};
        auto fileId      = addResult.value();
        auto tokenResult = lexer::tokenize(sourceMap, arena, fileId, diags);
        if (!tokenResult) {
            size_t errs = 0;
            for (auto &d : diags.all())
                if (d.severity == diagnostics::Severity::Error)
                    errs++;
            return {nullptr, nullptr, false, errs};
        }
        lexer::TokenStream tokens = std::move(tokenResult.value());
        auto *builder             = arena.make<ast::AstBuilder>(arena, interner);
        auto *prog                = arena.make<ast::ProgramNode>(arena);
        symbols::SymbolTable syms(arena, &interner);
        parser::Parser parser(&tokens, builder, &diags);
        parser::scan(parser, syms);
        *prog       = std::move(parser.program);
        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, prog, errs == 0, errs};
    }
};

static int count_decls(const ast::ProgramNode &prog) {
    int n = 0;
    for (auto id : prog.decls)
        if (id != ast::kInvalidDecl)
            n++;
    return n;
}

static const ast::TypeAliasDeclNode *get_alias(const ast::ProgramNode &prog, ast::AstBuilder *bld,
                                               int idx) {
    int n = 0;
    for (auto id : prog.decls) {
        if (id == ast::kInvalidDecl)
            continue;
        auto &decl  = bld->getDecl(id);
        auto *alias = std::get_if<ast::TypeAliasDeclNode>(&decl);
        if (!alias)
            continue;
        if (n++ == idx)
            return alias;
    }
    return nullptr;
}

// ── Tests ────────────────────────────────────────────────────

static void test_alias_simple() {
    AliasTest t;
    auto r = t.scan("alias cool = i32;");
    CHECK(r.ok, "alias cool = i32 scan succeeds");
    CHECK_EQ(count_decls(*r.program), 1, "1 decl");
    auto *alias = get_alias(*r.program, r.builder, 0);
    CHECK(alias != nullptr, "alias node exists");
    CHECK_EQ(alias->name, "cool", "name is 'cool'");
    CHECK(alias->target_type != ast::kInvalidTypeExpr, "target type is valid");
    CHECK(alias->generic_params.empty(), "no generic params");
}

static void test_alias_with_generic() {
    AliasTest t;
    auto r = t.scan("alias Option<T> = ?T;");
    CHECK(r.ok, "alias Option<T> scan succeeds");
    auto *alias = get_alias(*r.program, r.builder, 0);
    CHECK(alias != nullptr, "alias node exists");
    CHECK_EQ(alias->name, "Option", "name is 'Option'");
    CHECK_EQ(alias->generic_params.size(), 1u, "1 generic param");
    CHECK_EQ(alias->generic_params[0].name, "T", "generic param is 'T'");
}

static void test_alias_complex_type() {
    AliasTest t;
    auto r = t.scan("alias Callback = fn(i32): bool;");
    CHECK(r.ok, "alias Callback = fn(i32): bool scan succeeds");
    auto *alias = get_alias(*r.program, r.builder, 0);
    CHECK(alias != nullptr, "alias node exists");
    CHECK_EQ(alias->name, "Callback", "name is 'Callback'");
}

static void test_alias_ptr_type() {
    AliasTest t;
    auto r = t.scan("alias Ptr = *i32;");
    CHECK(r.ok, "alias Ptr = *i32 scan succeeds");
    auto *alias = get_alias(*r.program, r.builder, 0);
    CHECK(alias != nullptr, "alias node exists");
    CHECK_EQ(alias->name, "Ptr", "name is 'Ptr'");
}

static void test_alias_multiple() {
    AliasTest t;
    auto r = t.scan("alias A = i32;\nalias B = bool;");
    CHECK(r.ok, "multiple aliases scan succeeds");
    CHECK_EQ(count_decls(*r.program), 2, "2 decls");
    auto *a = get_alias(*r.program, r.builder, 0);
    CHECK(a != nullptr, "first alias exists");
    CHECK_EQ(a->name, "A", "first alias name");
    auto *b = get_alias(*r.program, r.builder, 1);
    CHECK(b != nullptr, "second alias exists");
    CHECK_EQ(b->name, "B", "second alias name");
}

static void test_pub_alias() {
    AliasTest t;
    auto r = t.scan("pub alias Cool = i32;");
    CHECK(r.ok, "pub alias scan succeeds");
    auto *alias = get_alias(*r.program, r.builder, 0);
    CHECK(alias != nullptr, "alias node exists");
    CHECK_EQ(alias->name, "Cool", "name is 'Cool'");
}

// ── Main ─────────────────────────────────────────────────────

static void test_alias() {
    test_alias_simple();
    test_alias_with_generic();
    test_alias_complex_type();
    test_alias_ptr_type();
    test_alias_multiple();
    test_pub_alias();
}

TEST_MAIN(alias)
