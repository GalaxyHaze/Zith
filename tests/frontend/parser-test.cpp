#include "frontend/parser/parse.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using common::memory::Arena;
using common::memory::StringInterner;
using generated_lexer::Lexer;
using generated_lexer::TokenStream;
using Parser = generated_parser::Parser<sample::ParseOutput>;

namespace {

bool printDiagnostics(std::string_view source,
                      const std::vector<sample::ParserDiagnostic> &diagnostics) {
    for (const auto &diagnostic : diagnostics) {
        std::cerr << "diag for '" << source << "' [" << diagnostic.span.start
                  << ',' << diagnostic.span.end << "] "
                  << diagnostic.message << '\n';
    }
    return !diagnostics.empty();
}

sample::ParseOutput parse(Arena &arena, StringInterner &strings,
                          std::string_view source) {
    Lexer lexer;
    TokenStream tokens = lexer.run(source, strings);
    Parser parser(arena);
    return hooks::parser::parseSource(parser, tokens, source);
}

bool parseAndCheck(std::string_view source, bool expectError,
                   bool expectImports = false) {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output = parse(arena, strings, source);
    const bool hasError = !output.diagnostics.empty();
    if (hasError != expectError) {
        std::cerr << "parse result mismatch for '" << source << "'\n";
        printDiagnostics(source, output.diagnostics);
        return false;
    }
    if (expectImports && output.imports.empty()) {
        std::cerr << "expected import metadata for '" << source << "'\n";
        return false;
    }
    if (output.ast.root == nullptr) {
        std::cerr << "program root missing for '" << source << "'\n";
        return false;
    }
    return true;
}

bool expectFunction() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings, "pub fn add(a: i32, b: i32) -> i32 { return a + b; }");
    if (!output.diagnostics.empty())
        return printDiagnostics("pub fn add", output.diagnostics);
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1 ||
        output.ast.root->body[0]->kind != generated_ast::NodeKind::Declaration)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    return declaration->kind == static_cast<int>(sample::DeclKind::Function) &&
           declaration->name == "add" &&
           declaration->visibility ==
               static_cast<int>(sample::VisibilityKind::Public) &&
           declaration->parameters.size() == 2 && declaration->body != nullptr;
}

bool expectImports() {
    struct Case {
        std::string_view source;
        bool expectError;
        bool expectFrom;
        bool expectExport;
        bool expectAsset;
        bool expectHeader;
        int depth;
        std::size_t selectors;
        std::string_view rawPath;
        std::string_view alias;
    };
    const Case cases[] = {
        {"import parent/sample;", false, false, false, false, false, 1, 0,
         "parent/sample", ""},
        {"import parent/sample as Alias;", false, false, false, false, false, 1,
         0, "parent/sample", "Alias"},
        {"from parent/sample { printf as show };", false, true, false, false,
         false, 1, 1, "parent/sample", ""},
        {"export parent/sample(..);", false, false, true, false, false, -1, 0,
         "parent/sample", ""},
        {"import \"stdio.h\" { printf };", false, false, false, false, true, 1,
         1, "\"stdio.h\"", ""},
        {"import assets/dat.json as Json;", false, false, false, true, false, 1,
         0, "assets/dat.json", "Json"},
        {"import asset assets/dat.json as Json;", false, false, false, true,
         false, 1, 0, "assets/dat.json", "Json"},
        {"import;", true, false, false, false, false, 1, 0, "", ""},
        {"import assets/dat.json;", false, false, false, true, false, 1, 0,
         "assets/dat.json", ""},
    };

    bool ok = true;
    for (const auto &test : cases) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, test.source);
        if (!output.diagnostics.empty() != test.expectError) {
            std::cerr << "import diagnostic mismatch for '" << test.source
                      << "'\n";
            printDiagnostics(test.source, output.diagnostics);
            ok = false;
            continue;
        }
        if (test.expectError)
            continue;
        if (output.imports.size() != 1) {
            std::cerr << "import count mismatch for '" << test.source << "'\n";
            ok = false;
            continue;
        }
        const auto &decl = output.imports.front();
        if (decl.rawPath != test.rawPath || decl.alias != test.alias ||
            decl.isFrom != test.expectFrom || decl.isExport != test.expectExport ||
            decl.isAsset != test.expectAsset || decl.isHeader != test.expectHeader ||
            decl.depth != test.depth || decl.selectors.size() != test.selectors) {
            std::cerr << "import fields mismatch for '" << test.source << "'\n";
            ok = false;
        }
    }
    return ok;
}

bool expectExpressions() {
    const std::string_view sources[] = {
        "let a = 1 + 2 * 3;",
        "let b = -x + call(1, 2)[0].field->other;",
        "let c = [1, 2, 3];",
        "let d = Point { x: 1, y: 2 };",
        "let e = a as i32;",
        "let f = if (cond) { a } else { b };",
        "pub fn g() { while (true) { break; } }",
        "pub fn h() { for { continue; } }",
    };
    bool ok = true;
    for (std::string_view source : sources) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, source);
        if (!output.diagnostics.empty()) {
            printDiagnostics(source, output.diagnostics);
            ok = false;
        }
    }
    return ok;
}

bool expectRecovery() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings, "garbage ??? ; fn valid() {}");
    if (output.diagnostics.empty()) {
        std::cerr << "expected garbage recovery diagnostics\n";
        return false;
    }
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1 ||
        output.ast.root->body[0]->kind != generated_ast::NodeKind::Declaration)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    return declaration->name == "valid";
}

bool expectSpans() {
    Arena arena;
    StringInterner strings(arena);
    const std::string_view source = "let value = 10;";
    sample::ParseOutput output = parse(arena, strings, source);
    if (!output.diagnostics.empty())
        return false;
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    if (declaration->span.start != 0 || declaration->span.end != source.size())
        return false;
    if (declaration->initializer == nullptr)
        return false;
    auto *initializer =
        static_cast<generated_ast::Expr *>(declaration->initializer);
    return initializer->span.end > initializer->span.start;
}

bool expectTypeKinds() {
    const std::string_view sources[] = {
        "type Ptr = *i32;",
        "type Opt = ?i32;",
        "type Slice = []i32;",
        "type Arr = [4]i32;",
        "type Opaque = raw opaque;",
        "type Mut = mut i32;",
    };
    bool ok = true;
    for (std::string_view source : sources) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, source);
        if (!output.diagnostics.empty()) {
            printDiagnostics(source, output.diagnostics);
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= parseAndCheck("alpha", true);
    ok &= parseAndCheck("", false);  // empty source with no parse error
    ok &= parseAndCheck("garbage", true);
    ok &= parseAndCheck("let x = 1 + 2 * 3;", false);
    ok &= parseAndCheck("import parent/sample;", false, true);
    ok &= expectFunction();
    ok &= expectImports();
    ok &= expectExpressions();
    ok &= expectRecovery();
    ok &= expectSpans();
    ok &= expectTypeKinds();
    if (!ok)
        return EXIT_FAILURE;
    std::cout << "parser-test: real parseSource parity checks passed\n";
    return EXIT_SUCCESS;
}
