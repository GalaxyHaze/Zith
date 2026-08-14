#include "frontend/parser/actions.hpp"

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
using generated_lexer::Token;
using generated_lexer::TokenStream;
using Parser = generated_parser::Parser<sample::ParseOutput>;

namespace hooks::parser {

using ParserT = generated_parser::Parser<sample::ParseOutput>;

generated_parser::Recovery recover(ParserT &parser, const Token &token) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    return token.kind == generated_lexer::TokenKind::Unknown
               ? generated_parser::Recovery::Abort
               : generated_parser::Recovery::Skip;
}

void beginModule(ParserT &parser, const Token &) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    ++parser.output().count;
}

void sawTopIdentifier(ParserT &parser, const Token &) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    ++parser.output().count;
}

void sawSemi(ParserT &, const Token &) {}
void enterType(ParserT &, const Token &) {}
void moduleName(ParserT &, const Token &) {}
void globalName(ParserT &, const Token &) {}
void leafExpr(ParserT &, const Token &) {}
void binaryExpr(ParserT &, const Token &) {}
void unaryExpr(ParserT &, const Token &) {}
void typeName(ParserT &, const Token &) {}

} // namespace hooks::parser

namespace {

bool expect(bool condition, std::string_view message, std::string_view source) {
    if (!condition)
        std::cerr << "parser-demo: " << message << " for '" << source << "'\n";
    return condition;
}

bool parseOne(std::string_view source) {
    Arena arena;
    StringInterner interner(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run(source, interner);
    Parser parser(arena);
    const auto result = parser.parse(tokens, source);

    std::cout << "parser-demo: '" << source << "' -> "
              << (result.isOk() ? "ok" : "error")
              << " count=" << parser.output().count
              << " stack=" << parser.stack().size()
              << "\n";
    return expect(result.isOk(), "parse failed", source);
}

} // namespace

int main() {
    bool ok = true;
    ok &= parseOne("alpha");
    ok &= parseOne("module alpha ;");
    ok &= parseOne("fn def [ * ] ;");
    ok &= parseOne("fn main() { print(42); }");
    ok &= parseOne("let x = 1 + 2 * 3;");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
