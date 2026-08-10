#include "frontend/lexer/lexer.hpp"
#include "common/arena.hpp"
#include "common/string-interner.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using generated_lexer::FormattedToken;
using generated_lexer::formatToken;
using generated_lexer::Lexer;
using generated_lexer::printToken;
using generated_lexer::Token;
using generated_lexer::TokenStream;
using generated_lexer::TokenKind;
using generated_lexer::tokenKindName;
using generated_lexer::terminalToken;
using memory::Arena;
using memory::StringInterner;

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
    Arena arena;
    StringInterner strings(arena);
    const TokenStream tokens = lexer.run(source, strings);
    if (tokens.size() != expected.size()) {
        std::cerr << label << ": expected " << expected.size() << " tokens, got "
                  << tokens.size() << "\n";
        return false;
    }

    size_t index = 0;
    for (const ExpectedToken &want : expected) {
        const Token &got = tokens[index];
        const std::string_view lexeme = tokens.lexeme(got);
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

bool expect_format(
    std::string_view label,
    std::string_view source,
    Span span,
    TokenKind kind,
    std::string_view expected
) {
    Arena arena;
    StringInterner strings(arena);
    const Token token(span, kind);
    Token token_with_lexeme = token;
    const std::string_view lexeme_source =
        span.end <= source.size()
            ? source.substr(span.start, span.end - span.start)
            : std::string_view{};
    token_with_lexeme.lexemeId = strings.intern(lexeme_source);
    const FormattedToken formatted = formatToken(token_with_lexeme, strings);
    const std::string actual =
        std::string(strings.lookup(formatted.kindId)) + "(" +
        std::string(strings.lookup(formatted.lexemeId)) + "): [" +
        std::to_string(formatted.span.start) + "," +
        std::to_string(formatted.span.end) + "]";
    if (actual != expected) {
        std::cerr << label << ": format mismatch\n"
                  << "  expected='" << expected << "'\n"
                  << "  got     ='" << actual << "'\n";
        return false;
    }

    FILE *file = tmpfile();
    if (file == nullptr) {
        std::cerr << label << ": tmpfile failed\n";
        return false;
    }
    printToken(file, formatted, strings);
    rewind(file);
    char buffer[512];
    const size_t n = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    if (n >= sizeof(buffer)) {
        std::cerr << label << ": print output too large\n";
        return false;
    }
    buffer[n] = '\0';
    const std::string_view print_output(buffer, n);
    if (print_output != std::string(expected) + "\n") {
        std::cerr << label << ": print mismatch\n"
                  << "  expected='" << expected << "\\n'\n"
                  << "  got     ='" << print_output << "'\n";
        return false;
    }
    return true;
}

bool expect_stream_helpers() {
    Lexer lexer;
    Arena arena;
    StringInterner strings(arena);
    TokenStream tokens = lexer.run("if + ;", strings);
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
    ok &= tokens.matchAllLexeme(";", "");

    tokens.reset();
    ok &= tokens.match(TokenKind::If);
    ok &= tokens.matchLexeme("+");
    ok &= tokens.matchLexeme(";");
    ok &= tokens.matchLexeme("");
    ok &= tokens.offset == 4;

    return ok;
}

bool expect_constructor_and_print() {
    const TokenStream empty;
    bool ok = empty.empty();
    ok &= empty.at(0).kind == TokenKind::Unknown;
    ok &= empty.absoluteSlice(0, 1).empty();
    ok &= empty.slice(0, 1).empty();
    ok &= empty.lexeme(empty.at(0)).empty();
    ok &= empty.lexeme(0).empty();

    const std::vector<Token> manual = {
        Token({0, 3}, TokenKind::LitVal),
        Token({4, 5}, TokenKind::Operators),
    };
    Arena manual_arena;
    StringInterner manual_strings(manual_arena);
    std::vector<Token> manual_mutable = manual;
    manual_mutable[0].lexemeId = manual_strings.intern("123");
    manual_mutable[1].lexemeId = manual_strings.intern("+");
    const TokenStream owned(std::move(manual_mutable), manual_strings);
    ok &= owned.size() == 2;
    ok &= owned.lexeme(owned[1]) == "+";
    ok &= owned.lexeme(owned[1].lexemeId) == "+";
    if (owned.size() != 2 ||
        owned.lexeme(owned[1]) != "+" ||
        owned.lexeme(owned[1].lexemeId) != "+")
        std::cerr << "owned lexeme/state mismatch\n";
    ok &= owned[1].lexemeId != 0;

    const std::array<Token, 2> manual_array = {
        Token({0, 1}, TokenKind::LitVal),
        Token({1, 2}, TokenKind::Operators),
    };
    const std::span<const Token> manual_span(manual_array);
    Arena span_arena;
    StringInterner span_strings(span_arena);
    Token span_tokens[2];
    span_tokens[0] = Token({0, 1}, TokenKind::LitVal);
    span_tokens[1] = Token({1, 2}, TokenKind::Operators);
    span_tokens[0].lexemeId = span_strings.intern("1");
    span_tokens[1].lexemeId = span_strings.intern("+");
    const std::span<const Token> span_span(span_tokens);
    const TokenStream from_span(span_span, span_strings);
    ok &= from_span.size() == 2;
    ok &= from_span.lexeme(from_span[1]) == "+";
    ok &= from_span.lexeme(from_span[1].lexemeId) == "+";
    ok &= from_span.lexeme(12345).empty();
    if (from_span.lexeme(from_span[1]) != "+" ||
        from_span.lexeme(from_span[1].lexemeId) != "+")
        std::cerr << "span lexeme mismatch\n";

    const FormattedToken stream_formatted = formatToken(from_span, 1);
    ok &= span_strings.lookup(stream_formatted.kindId) == "Operators";
    ok &= span_strings.lookup(stream_formatted.lexemeId) == "+";
    if (span_strings.lookup(stream_formatted.kindId) != "Operators" ||
        span_strings.lookup(stream_formatted.lexemeId) != "+")
        std::cerr << "stream formatted token mismatch\n";

    FILE *file = tmpfile();
    if (file == nullptr) return false;
    printToken(file, owned);
    rewind(file);
    char buffer[512];
    const size_t n1 = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    const std::string_view out(buffer, n1);
    ok &= out == "LitVal(123): [0,3]\nOperators(+): [4,5]\n";
    if (out != "LitVal(123): [0,3]\nOperators(+): [4,5]\n")
        std::cerr << "owned print mismatch: '" << out << "'\n";

    Arena print_arena;
    StringInterner print_strings(print_arena);
    file = tmpfile();
    if (file == nullptr) return false;
    printToken(file, "fn x", print_strings);
    rewind(file);
    const size_t n2 = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    const std::string_view out2(buffer, n2);
    ok &= out2 == "Fn(fn): [0,2]\nIdentifier(x): [3,4]\nEnd(): [4,4]\n";
    if (out2 != "Fn(fn): [0,2]\nIdentifier(x): [3,4]\nEnd(): [4,4]\n")
        std::cerr << "source print mismatch: '" << out2 << "'\n";
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
        {TokenKind::Operators, "-", '-'},
        {TokenKind::Operators, ">", '>'},
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

    ok &= expect_stream_helpers();
    ok &= expect_constructor_and_print();

    ok &= expect_format("print-keyword", "fn", {0, 2}, TokenKind::Fn, "Fn(fn): [0,2]");
    ok &= expect_format(
        "print-compound", "one += two", {4, 6}, TokenKind::Operators, "Operators(+=): [4,6]"
    );
    ok &= expect_format(
        "print-punc", "{;}", {1, 2}, TokenKind::Punctuation, "Punctuation(;): [1,2]"
    );
    ok &= expect_format(
        "print-eof", "abc", {3, 3}, TokenKind::End, "End(): [3,3]"
    );
    ok &= expect_format(
        "print-out-of-range", "abc", {3, 4}, TokenKind::Unknown, "Unknown(): [3,4]"
    );

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
