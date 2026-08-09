#include "frontend/lexer/lexer.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <vector>

using generated_lexer::Lexer;
using generated_lexer::Token;
using generated_lexer::TokenKind;
using generated_lexer::tokenKindName;

namespace {

struct ExpectedToken {
    TokenKind kind = TokenKind::Unknown;
    std::string_view lexeme;
    char punc = 0;
};

bool expect_tokens(
    std::string_view label,
    std::string_view source,
    std::initializer_list<ExpectedToken> expected
) {
    Lexer lexer;
    const std::vector<Token> tokens = lexer.run(source);
    if (tokens.size() != expected.size()) {
        std::cerr << label << ": expected " << expected.size() << " tokens, got "
                  << tokens.size() << "\n";
        return false;
    }

    size_t index = 0;
    for (const ExpectedToken &want : expected) {
        const Token &got = tokens[index];
        const std::string_view lexeme = lexer.span_slice(source, got.span);
        if (got.kind != want.kind || lexeme != want.lexeme || got.punc != want.punc) {
            std::cerr << label << ": token " << index << " mismatch\n"
                      << "  expected kind=" << tokenKindName(want.kind)
                      << " lexeme='" << want.lexeme << "' punc="
                      << static_cast<int>(want.punc) << "\n"
                      << "  got      kind=" << tokenKindName(got.kind)
                      << " lexeme='" << lexeme << "' punc="
                      << static_cast<int>(got.punc) << "\n";
            return false;
        }
        ++index;
    }

    return true;
}

} // namespace

int main() {
    bool ok = true;

    ok &= expect_tokens("decimal", "123", {
        {TokenKind::LitVal, "123"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("decimal-underscore", "1_000", {
        {TokenKind::LitVal, "1_000"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("decimal-groups", "12_34_56", {
        {TokenKind::LitVal, "12_34_56"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("decimal-float-underscore", "1_234.5_6", {
        {TokenKind::LitVal, "1_234.5_6"},
        {TokenKind::End, ""},
    });

    ok &= expect_tokens("hex", "0xFF", {
        {TokenKind::LitVal, "0xFF"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("hex-underscore", "0xFF_A0", {
        {TokenKind::LitVal, "0xFF_A0"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("binary-underscore", "0b1010_0011", {
        {TokenKind::LitVal, "0b1010_0011"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("octal-underscore", "0c7_1", {
        {TokenKind::LitVal, "0c7_1"},
        {TokenKind::End, ""},
    });

    ok &= expect_tokens("invalid-trailing-underscore", "1_", {
        {TokenKind::Unknown, "1_"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("invalid-prefix-underscore", "0x_FF", {
        {TokenKind::Unknown, "0x_FF"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("invalid-repeated-underscore", "0b__1", {
        {TokenKind::Unknown, "0b__1"},
        {TokenKind::End, ""},
    });

    ok &= expect_tokens("arrow-default", "->", {
        {TokenKind::Operators, "->"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("compound-operators", "+= >>= <<=", {
        {TokenKind::Operators, "+="},
        {TokenKind::Operators, ">>="},
        {TokenKind::Operators, "<<="},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("single-char-punc", "+;", {
        {TokenKind::Operators, "+", '+'},
        {TokenKind::Punctuation, ";", ';'},
        {TokenKind::End, ""},
    });

    ok &= expect_tokens("string-escapes", R"("a\"b\\c\n")", {
        {TokenKind::LitVal, R"("a\"b\\c\n")"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("string-invalid-escape", R"("\q")", {
        {TokenKind::Unknown, R"("\q")"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("string-unterminated", R"("abc)", {
        {TokenKind::Unknown, R"("abc)"},
        {TokenKind::End, ""},
    });

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
