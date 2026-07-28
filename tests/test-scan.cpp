#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
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

struct ScanTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;
    lexer::TokenStream tokens;

    ScanTest() : interner(arena), diags(arena), tokens{} {}

    struct Result {
        ast::AstBuilder *builder;
        ast::ProgramNode *program;
        parser::ScanResult scanResult;
        bool ok;
        size_t errorCount;
    };

    Result scan(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {nullptr, nullptr, parser::ScanResult{arena}, false, 0};
        auto fileId      = addResult.value();
        auto tokenResult = lexer::tokenize(sourceMap, arena, fileId, diags);
        if (!tokenResult) {
            size_t errs = 0;
            for (auto &d : diags.all())
                if (d.severity == diagnostics::Severity::Error)
                    errs++;
            return {nullptr, nullptr, parser::ScanResult{arena}, false, errs};
        }
        tokens        = std::move(tokenResult.value());
        auto *builder = arena.make<ast::AstBuilder>(arena, interner);
        auto *prog    = arena.make<ast::ProgramNode>(arena);
        symbols::SymbolTable syms(arena, &interner);
        parser::Parser parser(&tokens, builder, &diags);
        auto scanResult = parser::scan(parser, syms);
        parser.expandBodies(scanResult, syms);
        *prog = std::move(parser.program);

        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, prog, std::move(scanResult), errs == 0, errs};
    }
};

// ── Helpers ────────────────────────────────────────────────────────

template <typename T>
static const T *get_decl_of(const ast::ProgramNode &prog, ast::AstBuilder *bld,
                            std::string_view name) {
    for (auto id : prog.decls) {
        if (id == ast::kInvalidDecl)
            continue;
        auto &decl = bld->getDecl(id);
        auto *p    = std::get_if<T>(&decl);
        if (p && p->name == name)
            return p;
    }
    return nullptr;
}

// ── Enum tests ─────────────────────────────────────────────────────

static void test_enum_empty() {
    ScanTest t;
    auto r = t.scan("enum Foo { }");
    CHECK(r.ok, "empty enum");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Foo");
    CHECK(e != nullptr, "enum node exists");
    CHECK_EQ(r.scanResult.enums.size(), 1, "1 enum entry");
}

static void test_enum_variants() {
    ScanTest t;
    auto r = t.scan("enum Color { Red, Green, Blue, }");
    CHECK(r.ok, "enum with variants");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Color");
    CHECK(e != nullptr, "enum node exists");
}

static void test_enum_tuple_variant() {
    ScanTest t;
    auto r = t.scan("enum Opt { Some(Int, String), None, }");
    CHECK(r.ok, "enum with tuple variant");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Opt");
    CHECK(e != nullptr, "enum node exists");
}

static void test_enum_struct_variant() {
    ScanTest t;
    auto r = t.scan("enum Node { Leaf { val: Int }, Branch { left: Node, right: Node }, }");
    CHECK(r.ok, "enum with struct variant");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Node");
    CHECK(e != nullptr, "enum node exists");
}

static void test_enum_discriminant() {
    ScanTest t;
    auto r = t.scan("enum Color { Red = 0, Green = 1, Blue = 2, }");
    CHECK(r.ok, "enum with discriminant");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Color");
    CHECK(e != nullptr, "enum node exists");
}

static void test_enum_method() {
    ScanTest t;
    auto r = t.scan("enum Foo { Bar, fn is_bar(): Bool; }");
    CHECK(r.ok, "enum with method");
    auto *e = get_decl_of<ast::EnumDeclNode>(*r.program, r.builder, "Foo");
    CHECK(e != nullptr, "enum node exists");
    CHECK(r.scanResult.fns.size() >= 1, "at least 1 fn entry");
}

// ── Union tests ────────────────────────────────────────────────────

static void test_union_basic() {
    ScanTest t;
    auto r = t.scan("union Value { Int, String, Float, }");
    CHECK(r.ok, "union with types");
    auto *u = get_decl_of<ast::UnionDeclNode>(*r.program, r.builder, "Value");
    CHECK(u != nullptr, "union node exists");
}

static void test_union_typed_variant() {
    ScanTest t;
    auto r = t.scan("union Value { int_val: Int, str_val: String, }");
    CHECK(r.ok, "union with typed variants");
    auto *u = get_decl_of<ast::UnionDeclNode>(*r.program, r.builder, "Value");
    CHECK(u != nullptr, "union node exists");
}

static void test_union_method() {
    ScanTest t;
    auto r = t.scan("union Foo { Int, fn to_int(): Int; }");
    CHECK(r.ok, "union with method");
    auto *u = get_decl_of<ast::UnionDeclNode>(*r.program, r.builder, "Foo");
    CHECK(u != nullptr, "union node exists");
    CHECK(r.scanResult.fns.size() >= 1, "at least 1 fn entry");
}

// ── Component tests ────────────────────────────────────────────────

static void test_component_empty() {
    ScanTest t;
    auto r = t.scan("component App { }");
    CHECK(r.ok, "empty component");
    auto *c = get_decl_of<ast::ComponentDeclNode>(*r.program, r.builder, "App");
    CHECK(c != nullptr, "component node exists");
}

static void test_component_fields() {
    ScanTest t;
    auto r = t.scan("component App { name: String, age: Int, }");
    CHECK(r.ok, "component with fields");
    auto *c = get_decl_of<ast::ComponentDeclNode>(*r.program, r.builder, "App");
    CHECK(c != nullptr, "component node exists");
}

static void test_component_method() {
    ScanTest t;
    auto r = t.scan("component App { fn greet(name: String): String { name } }");
    CHECK(r.ok, "component with method");
    auto *c = get_decl_of<ast::ComponentDeclNode>(*r.program, r.builder, "App");
    CHECK(c != nullptr, "component node exists");
    CHECK(r.scanResult.fns.size() >= 1, "at least 1 fn entry");
}

// ── Top-level let/global/const tests ───────────────────────────────

static void test_top_level_global() {
    ScanTest t;
    auto r = t.scan("global x: Int = 42;");
    CHECK(r.ok, "top-level global");
}

static void test_top_level_const() {
    ScanTest t;
    auto r = t.scan("const x: Int = 42;");
    CHECK(r.ok, "top-level const");
}

static void test_top_level_let_error() {
    ScanTest t;
    auto r = t.scan("let x = 42;");
    CHECK(!r.ok, "top-level let errors");
    bool found = false;
    for (auto &d : t.diags.all())
        if (d.code == diagnostics::err::TopLevelLetNotAllowed)
            found = true;
    CHECK(found, "E1007 reported for top-level let");
}

static void test_top_level_var_error() {
    ScanTest t;
    auto r = t.scan("var x = 42;");
    CHECK(!r.ok, "top-level var errors");
    bool found = false;
    for (auto &d : t.diags.all())
        if (d.code == diagnostics::err::TopLevelLetNotAllowed)
            found = true;
    CHECK(found, "E1007 reported for top-level var");
}

// ── Duplicate detection tests ──────────────────────────────────────

static void test_duplicate_enum_variant() {
    ScanTest t;
    auto r = t.scan("enum Foo { A, A, }");
    CHECK(!r.ok, "duplicate enum variant errors");
    bool found = false;
    for (auto &d : t.diags.all())
        if (d.code == diagnostics::err::DuplicateDecl)
            found = true;
    CHECK(found, "DuplicateDecl reported for duplicate variant");
}

static void test_duplicate_union_variant() {
    ScanTest t;
    auto r = t.scan("union Foo { i32, i32, }");
    CHECK(!r.ok, "duplicate union variant errors");
    bool found = false;
    for (auto &d : t.diags.all())
        if (d.code == diagnostics::err::DuplicateDecl)
            found = true;
    CHECK(found, "DuplicateDecl reported for duplicate union variant");
}

// ── Struct method test ─────────────────────────────────────────────

static void test_struct_method() {
    ScanTest t;
    auto r = t.scan("struct Foo { fn bar(): Int; }");
    CHECK(r.ok, "struct with method");
    auto *s = get_decl_of<ast::StructDeclNode>(*r.program, r.builder, "Foo");
    CHECK(s != nullptr, "struct node exists");
    CHECK(r.scanResult.fns.size() >= 1, "at least 1 fn entry");
}

static void test_struct_method_body() {
    ScanTest t;
    auto r = t.scan("struct Foo { fn bar(): Int { 42 } }");
    CHECK(r.ok, "struct with method body");
    auto *s = get_decl_of<ast::StructDeclNode>(*r.program, r.builder, "Foo");
    CHECK(s != nullptr, "struct node exists");
    CHECK(r.scanResult.fns.size() >= 1, "at least 1 fn entry");
}

// ── Extern fn tests ──────────────────────────────────────────────

static void test_extern_fn_basic() {
    ScanTest t;
    auto r = t.scan("extern fn write()");
    CHECK(r.ok, "extern fn basic");
    auto *fn = get_decl_of<ast::FnDeclNode>(*r.program, r.builder, "write");
    CHECK(fn != nullptr, "extern fn node exists");
    CHECK(fn->is_extern, "fn is marked extern");
    CHECK(fn->body == ast::kInvalidExpr, "extern fn has no body");
}

static void test_extern_fn_with_params() {
    ScanTest t;
    auto r = t.scan("extern fn write(fd: i32, msg: *char, len: u64): i64");
    CHECK(r.ok, "extern fn with params");
    auto *fn = get_decl_of<ast::FnDeclNode>(*r.program, r.builder, "write");
    CHECK(fn != nullptr, "extern fn node exists");
    CHECK(fn->is_extern, "fn is marked extern");
    CHECK_EQ(fn->params.size(), 3u, "3 params");
}

static void test_extern_fn_with_return() {
    ScanTest t;
    auto r = t.scan("extern fn getchar(): i32");
    CHECK(r.ok, "extern fn with return type");
    auto *fn = get_decl_of<ast::FnDeclNode>(*r.program, r.builder, "getchar");
    CHECK(fn != nullptr, "extern fn node exists");
    CHECK(fn->is_extern, "fn is marked extern");
    CHECK(fn->return_type != ast::kInvalidTypeExpr, "has return type");
}

static void test_word_declaration() {
    ScanTest t;
    auto r = t.scan("nop SELECT;");
    CHECK(r.ok, "word declaration scans");
    auto *word = get_decl_of<ast::WordDeclNode>(*r.program, r.builder, "SELECT");
    CHECK(word != nullptr, "word node exists");
    CHECK(word && word->category == ast::WordCategory::Nop, "word category is preserved");
}

static void test_context_declaration() {
    ScanTest t;
    auto r = t.scan("context SQL { nop SELECT; }");
    CHECK(r.ok, "context declaration scans");
    auto *context = get_decl_of<ast::ContextDeclNode>(*r.program, r.builder, "SQL");
    CHECK(context != nullptr, "context node exists");
    CHECK(context && context->decls.size() == 1, "context retains nested word declaration");
}

// ── Runner ─────────────────────────────────────────────────────────

static void test_scan() {
    test_enum_empty();
    test_enum_variants();
    test_enum_tuple_variant();
    test_enum_struct_variant();
    test_enum_discriminant();
    test_enum_method();

    test_union_basic();
    test_union_typed_variant();
    test_union_method();

    test_component_empty();
    test_component_fields();
    test_component_method();

    test_top_level_global();
    test_top_level_const();
    test_top_level_let_error();
    test_top_level_var_error();

    test_duplicate_enum_variant();
    test_duplicate_union_variant();

    test_struct_method();
    test_struct_method_body();

    test_extern_fn_basic();
    test_extern_fn_with_params();
    test_extern_fn_with_return();
    test_word_declaration();
    test_context_declaration();
}

TEST_MAIN(scan)
