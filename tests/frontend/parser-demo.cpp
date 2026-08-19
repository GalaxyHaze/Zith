#include "frontend/parser/parse.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::memory::Arena;
using common::memory::StringInterner;
using generated_lexer::Lexer;
using generated_lexer::TokenStream;
using Parser = generated_parser::Parser<sample::ParseOutput>;

namespace {

bool expect(bool condition, std::string_view message, std::string_view source) {
    if (!condition)
        std::cerr << "parser-demo: " << message << " for '" << source << "'\n";
    return condition;
}

bool parseOne(std::string_view source, bool expectError = false) {
    Arena arena;
    StringInterner interner(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run(source, interner);
    Parser parser(arena);
    sample::ParseOutput output =
        hooks::parser::parseSource(parser, tokens, source);

    const bool error = !output.diagnostics.empty();
    const std::size_t declarations =
        output.ast.root != nullptr ? output.ast.root->body.size() : 0;
    std::cout << "parser-demo: '" << source << "' -> "
              << (error ? "error" : "ok") << " nodes=" << declarations
              << " imports=" << output.imports.size() << "\n";
    return expect(error == expectError, "parse result mismatch", source);
}

} // namespace

int main() {
    bool ok = true;
    ok &= parseOne("alpha", true);
    ok &= parseOne("fn def() { }");
    ok &= parseOne("let x = 1 + 2 * 3;");
    ok &= parseOne("import parent/sample;");
    ok &= parseOne("import parent/sample { printf };");
    ok &= parseOne("from parent/sample as Alias;");
    ok &= parseOne("export parent/sample(..);");
    ok &= parseOne("import \"stdio.h\" { printf };");
    ok &= parseOne("import assets/dat.json as Json;");
    ok &= parseOne("import asset assets/dat.json as Json;");
    ok &= parseOne("import assets/dat.json;");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
