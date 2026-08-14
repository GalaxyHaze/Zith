#include "frontend/lexer/lexer.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::memory::Arena;
using common::memory::StringInterner;
using generated_lexer::Lexer;
using generated_lexer::Token;
using generated_lexer::TokenKind;
using generated_lexer::TokenStream;
using generated_lexer::tokenKindName;

namespace {

void printToken(std::string_view lexeme, const Token &token) {
    std::cout << tokenKindName(token.kind)
              << "(" << lexeme << ")"
              << " [" << token.span.start << "," << token.span.end << "]"
              << "\n";
}

} // namespace

int main() {
    constexpr std::string_view source =
        "fn main(x) { return \"hi\"; }\n0x2A else -> if";

    Arena arena;
    StringInterner interner(arena);
    Lexer lexer;
    const TokenStream tokens = lexer.run(source, interner);

    std::size_t count = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const Token &token = tokens[index];
        const std::string_view lexeme = tokens.lexeme(token);
        printToken(lexeme, token);
        ++count;
    }

    const bool sawKeyword = count > 0;
    std::cout << "lexer-demo: tokenized " << count << " tokens\n";
    return sawKeyword ? EXIT_SUCCESS : EXIT_FAILURE;
}
