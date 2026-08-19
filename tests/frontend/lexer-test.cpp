#include "frontend/lexer/lexer.hpp"
#include "frontend/lexer/keyword-table.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <initializer_list>

using generated_lexer::Lexer;
using generated_lexer::Token;
using generated_lexer::TokenStream;
using generated_lexer::TokenKind;
using generated_lexer::tokenKindName;
using generated_lexer::terminalToken;
using generated_lexer::lookupKeyword;
using generated_lexer::detail::keyword_meta;

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
    const TokenStream tokens = lexer.run(source);
    if (tokens.size() != expected.size()) {
        std::cerr << label << ": expected " << expected.size() << " tokens, got "
                  << tokens.size() << "\n";
        return false;
    }

    size_t index = 0;
    for (const ExpectedToken &want : expected) {
        const Token &got = tokens[index];
        const std::string_view lexeme =
            source.substr(got.span.start, got.span.end - got.span.start);
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

bool expect_stream_helpers() {
    Lexer lexer;
    constexpr std::string_view source = "if + ;";
    TokenStream tokens = lexer.run(source);
    bool ok = tokens.size() == 4 && !tokens.empty();
    ok &= tokens[0].kind == TokenKind::If && tokens[1].kind == TokenKind::Operators &&
          tokens[2].kind == TokenKind::Punctuation && tokens[3].kind == TokenKind::End;
    ok &= tokens.at(tokens.size() + 10).kind == TokenKind::Unknown;
    ok &= terminalToken().kind == TokenKind::Unknown;

    ok &= tokens.current().kind == TokenKind::If;
    ok &= tokens.peek().kind == TokenKind::If;
    ok &= tokens.hasNext();
    tokens.advance();
    ok &= tokens.current().kind == TokenKind::Operators;
    ok &= tokens.match(TokenKind::Operators);
    ok &= tokens.current().kind == TokenKind::Punctuation;
    ok &= tokens.match(TokenKind::Punctuation, TokenKind::End);
    ok &= !tokens.hasNext();
    ok &= tokens.current().kind == TokenKind::Unknown;
    ok &= !tokens.match(TokenKind::End);
    tokens.reset();
    ok &= tokens.current().kind == TokenKind::If;

    const auto all = tokens.absoluteSlice(0, tokens.size());
    ok &= all.size() == tokens.size();
    ok &= tokens.absoluteSlice(2, 2).empty();
    ok &= tokens.absoluteSlice(2, 99).size() == 2;
    ok &= tokens.absoluteSlice(100, 200).empty();
    ok &= tokens.slice(0, 2).size() == 2;
    ok &= tokens.slice(3, 1).size() == 1;
    ok &= tokens.slice(0, 99).size() == 4;

    tokens.advance();
    ok &= tokens.slice(0, 1).size() == 1;
    ok &= tokens.matchAll(TokenKind::Operators, TokenKind::Punctuation, TokenKind::End);
    ok &= !tokens.hasNext();

    tokens.reset();
    ok &= tokens.matchAll(TokenKind::If, TokenKind::Operators);
    ok &= tokens.current().kind == TokenKind::Punctuation;
    const size_t before_fail = tokens.offset;
    ok &= !tokens.matchAll(TokenKind::Punctuation, TokenKind::End, TokenKind::If);
    ok &= tokens.offset == before_fail;
    return ok;
}

bool expect_constructor() {
    const TokenStream empty;
    bool ok = empty.empty();
    ok &= empty.at(0).kind == TokenKind::Unknown;
    ok &= empty.absoluteSlice(0, 1).empty();
    ok &= empty.slice(0, 1).empty();

    const std::vector<Token> manual = {
        Token({0, 3}, TokenKind::LitVal),
        Token({4, 5}, TokenKind::Operators),
    };
    const TokenStream owned(std::move(manual));
    ok &= owned.size() == 2;

    const std::array<Token, 2> manual_array = {
        Token({0, 1}, TokenKind::LitVal),
        Token({1, 2}, TokenKind::Operators),
    };
    const std::span<const Token> manual_span(manual_array);
    const TokenStream from_span(manual_span);
    ok &= from_span.size() == 2;
    return ok;
}

bool expect_all_keywords() {
    bool ok = true;
    constexpr std::string_view spellings[] = {
        "as", "use", "import", "from", "export", "asset",
        "type", "alias", "auto", "dyn", "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128", "f32", "f64", "bool", "char", "void",
        "struct", "component", "enum", "union", "typedef", "true", "false",
        "null", "unknown", "invalid", "never", "unsafe", "extends",
        "fn", "mod", "extern", "macro", "context", "with", "catch", "spawn",
        "await", "async",
        "let", "var", "const", "global", "lend", "share", "view", "unique",
        "belong", "return", "break", "continue", "yield",
        "marker", "stackful", "dock", "jump", "tag", "state", "enter", "leave",
        "pub", "if", "else", "for", "in", "when", "match", "while", "flow",
        "throw", "fail", "drop", "require", "must", "is", "raw", "mut",
        "trait", "interface", "implement", "impl", "word", "prefix", "suffix",
        "infix", "nop", "and", "or", "not", "xor",
    };
    if (std::size(spellings) != keyword_meta.size()) {
        std::cerr << "all-keywords: expected " << keyword_meta.size()
                  << " spellings, got " << std::size(spellings) << "\n";
        return false;
    }
    for (size_t i = 0; i < std::size(spellings); ++i) {
        std::string_view word = spellings[i];
        const auto &meta = keyword_meta[i];
        if (lookupKeyword(word) != static_cast<TokenKind>(meta.token_kind)) {
            std::cerr << "all-keywords: " << word
                      << " expected kind="
                      << static_cast<int>(meta.token_kind)
                      << " got=" << static_cast<int>(lookupKeyword(word)) << "\n";
            ok = false;
        }

        char prefix_buf[10 + 1];
        char suffix_buf[1 + 9];
        std::string_view prefix;
        std::string_view suffix;
        const size_t len = word.size();
        for (size_t k = 0; k < len; ++k) prefix_buf[k] = word[k];
        prefix_buf[len] = '@';
        prefix = std::string_view(prefix_buf, len + 1);

        suffix_buf[0] = '@';
        for (size_t k = 0; k < len; ++k) suffix_buf[k + 1] = word[k];
        suffix = std::string_view(suffix_buf, len + 1);
        if (lookupKeyword(prefix) != TokenKind::Identifier) {
            std::cerr << "all-keywords: " << word
                      << " prefix-miss returned keyword\n";
            ok = false;
        }
        if (lookupKeyword(suffix) != TokenKind::Identifier) {
            std::cerr << "all-keywords: " << word
                      << " suffix-miss returned keyword\n";
            ok = false;
        }
    }
    return ok;
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

    ok &= expect_tokens("arrow-from-rules", "->", {
        {TokenKind::Operators, "->"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("compound-operators", "+= >>= <<=", {
        {TokenKind::Operators, "+="},
        {TokenKind::Operators, ">>="},
        {TokenKind::Operators, "<<="},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("compound-assign-all", "+= -= *= /= %= <<= >>= &= |= ^=", {
        {TokenKind::Operators, "+="},
        {TokenKind::Operators, "-="},
        {TokenKind::Operators, "*="},
        {TokenKind::Operators, "/="},
        {TokenKind::Operators, "%="},
        {TokenKind::Operators, "<<="},
        {TokenKind::Operators, ">>="},
        {TokenKind::Operators, "&="},
        {TokenKind::Operators, "|="},
        {TokenKind::Operators, "^="},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("bitwise-compound-maximal-munch", "&=", {
        {TokenKind::Operators, "&="},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("arrow-maximal-munch", "->", {
        {TokenKind::Operators, "->"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("single-char-punc", "+;", {
        {TokenKind::Operators, "+", '+'},
        {TokenKind::Punctuation, ";", ';'},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("import-keywords", "import from export asset as", {
        {TokenKind::Using, "import"},
        {TokenKind::Using, "from"},
        {TokenKind::Using, "export"},
        {TokenKind::Using, "asset"},
        {TokenKind::As, "as"},
        {TokenKind::End, ""},
    });
    ok &= expect_tokens("dotdot-operator", "..", {
        {TokenKind::Operators, ".."},
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

    ok &= expect_stream_helpers();
    ok &= expect_all_keywords();
    ok &= expect_constructor();

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
