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

struct StructTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;
    lexer::TokenStream tokens;

    StructTest() : interner(arena), diags(arena), tokens{} {}

    struct Result {
        ast::AstBuilder *builder;
        ast::ProgramNode *program;
        bool ok;
        size_t errorCount;
    };

    Result scanAndExpand(std::string_view input) {
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
        tokens        = std::move(tokenResult.value());
        auto *builder = arena.make<ast::AstBuilder>(arena, interner);
        auto *prog    = arena.make<ast::ProgramNode>(arena);
        symbols::SymbolTable syms(arena, &interner);
        parser::Parser parser(&tokens, builder, &diags);
        auto scanResult = parser::scan(parser, syms);
        *prog           = std::move(parser.program);

        // Expand bodies
        parser.program = std::move(*prog);
        parser.expandBodies(scanResult, syms);
        *prog = std::move(parser.program);

        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, prog, errs == 0, errs};
    }
};

static const ast::StructDeclNode *get_struct(const ast::ProgramNode &prog, ast::AstBuilder *bld) {
    for (auto id : prog.decls) {
        if (id == ast::kInvalidDecl)
            continue;
        auto &decl = bld->getDecl(id);
        auto *s    = std::get_if<ast::StructDeclNode>(&decl);
        if (s)
            return s;
    }
    return nullptr;
}

// ── Tests ────────────────────────────────────────────────────

static void test_struct_fields() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { x: i32, y: bool, }");
    CHECK(r.ok, "struct with fields");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 2, "2 fields");
    CHECK_EQ(s->fields[0].name, "x", "first field name");
    CHECK(s->fields[0].type_expr != ast::kInvalidTypeExpr, "first field has type");
    CHECK_EQ(s->fields[0].bind, ast::FieldBind::Auto, "first field bind auto");
    CHECK_EQ(s->fields[1].name, "y", "second field name");
    CHECK(s->fields[1].type_expr != ast::kInvalidTypeExpr, "second field has type");
}

static void test_struct_empty() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { }");
    CHECK(r.ok, "empty struct");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 0, "0 fields");
}

static void test_struct_no_type() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { x, }");
    CHECK(r.ok, "field without type");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 1, "1 field");
    CHECK_EQ(s->fields[0].name, "x", "field name");
    CHECK(s->fields[0].type_expr == ast::kInvalidTypeExpr, "no type annotation");
}

static void test_struct_default() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { x: i32 = 42, }");
    CHECK(r.ok, "field with default");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 1, "1 field");
    CHECK_EQ(s->fields[0].name, "x", "field name");
    CHECK(s->fields[0].default_value != ast::kInvalidExpr, "has default");
}

static void test_struct_destructure() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { [a, b]: i32, }");
    CHECK(r.ok, "destructured fields");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 2, "2 fields");
    CHECK_EQ(s->fields[0].name, "a", "first field name");
    CHECK_EQ(s->fields[1].name, "b", "second field name");
    CHECK(s->fields[0].type_expr != ast::kInvalidTypeExpr, "first field has type");
    CHECK(s->fields[0].type_expr == s->fields[1].type_expr, "same type for both");
}

static void test_struct_qualifier() {
    StructTest t;
    auto r = t.scanAndExpand("struct Foo { const x: f32, }");
    CHECK(r.ok, "field with const qualifier");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    CHECK_EQ(s->fields.size(), 1, "1 field");
    CHECK_EQ(s->fields[0].name, "x", "field name");
    CHECK_EQ(s->fields[0].bind, ast::FieldBind::Const, "const bind");
}

static void test_struct_extends() {
    StructTest t;
    auto r = t.scanAndExpand("struct Bar { z: f32, }\nstruct Foo extends Bar { x: i32, }");
    CHECK(r.ok, "struct with extends");
    auto *s = get_struct(*r.program, r.builder);
    CHECK(s != nullptr, "struct node exists");
    if (s->name == "Bar") {
        s = nullptr;
        for (auto id : r.program->decls) {
            if (id == ast::kInvalidDecl)
                continue;
            auto &decl = r.builder->getDecl(id);
            auto *s2   = std::get_if<ast::StructDeclNode>(&decl);
            if (s2 && s2->name == "Foo") {
                s = s2;
                break;
            }
        }
        CHECK(s != nullptr, "Foo struct found");
    }
    CHECK(s->extends_parent != ast::kInvalidTypeExpr, "has extends parent");
}

static void test_struct() {
    test_struct_fields();
    test_struct_empty();
    test_struct_no_type();
    test_struct_default();
    test_struct_destructure();
    test_struct_qualifier();
    test_struct_extends();
}

// ── Main ─────────────────────────────────────────────────────

TEST_MAIN(struct)
