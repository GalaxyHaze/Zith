#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "memory/arena.hpp"
#include "memory/source-map.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <string_view>

using namespace zith;

#define CHECK_GE(a, b, msg) CHECK((a) >= (b), msg)

struct LexerTest {
    memory::Arena arena;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;

    LexerTest() : diags(arena) {}

    struct Result {
        lexer::TokenStream stream;
        bool ok;
        size_t errorCount;
    };

    Result lex(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {{}, false, 0};
        auto result = lexer::tokenize(sourceMap, arena, addResult.value(), diags);
        bool ok     = static_cast<bool>(result);
        size_t errs = 0;
        for (auto &d : diags.all()) {
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        }
        return {ok ? result.value() : lexer::TokenStream{}, ok, errs};
    }
};

static void test_decimal_42() {
    LexerTest t;
    auto r = t.lex("42");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.len, 2u, "2 tokens (LitVal + End)");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "42", "lexeme is '42'");
}

static void test_decimal_0() {
    LexerTest t;
    auto r = t.lex("0");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0", "lexeme is '0'");
}

static void test_float_314() {
    LexerTest t;
    auto r = t.lex("3.14");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "3.14", "lexeme is '3.14'");
}

static void test_float_dot_only() {
    LexerTest t;
    auto r = t.lex("0.");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0", "lexeme is '0' (no dot)");
}

static void test_hex_ff() {
    LexerTest t;
    auto r = t.lex("0xFF");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0xFF", "lexeme is '0xFF'");
}

static void test_hex_upper() {
    LexerTest t;
    auto r = t.lex("0XDEAD");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0XDEAD", "lexeme is '0XDEAD'");
}

static void test_hex_no_digits() {
    LexerTest t;
    auto r = t.lex("0x");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_hex_invalid_digit() {
    LexerTest t;
    auto r = t.lex("0xG");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_octal_77() {
    LexerTest t;
    auto r = t.lex("0c77");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0c77", "lexeme is '0c77'");
}

static void test_octal_no_digits() {
    LexerTest t;
    auto r = t.lex("0c");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_octal_invalid_digit() {
    LexerTest t;
    auto r = t.lex("0c89");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_octal_invalid_after_valid() {
    LexerTest t;
    auto r = t.lex("0c08");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_binary_1010() {
    LexerTest t;
    auto r = t.lex("0b1010");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "0b1010", "lexeme is '0b1010'");
}

static void test_binary_no_digits() {
    LexerTest t;
    auto r = t.lex("0b");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_binary_invalid_digit() {
    LexerTest t;
    auto r = t.lex("0b2");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_string_simple() {
    LexerTest t;
    auto r = t.lex("\"hello\"");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "\"hello\"", "lexeme includes quotes");
}

static void test_string_empty() {
    LexerTest t;
    auto r = t.lex("\"\"");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
}

static void test_string_escape_n() {
    LexerTest t;
    auto r = t.lex("\"a\\nb\"");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "\"a\\nb\"", "lexeme preserved");
}

static void test_string_escape_invalid() {
    LexerTest t;
    auto r = t.lex("\"a\\xb\"");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_string_unterminated() {
    LexerTest t;
    auto r = t.lex("\"hello");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_string_backslash_at_eof() {
    LexerTest t;
    auto r = t.lex("\"\\");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_char_literal() {
    LexerTest t;
    auto r = t.lex("'a'");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::LitVal, "token is LitVal");
}

static void test_comment_line() {
    LexerTest t;
    auto r = t.lex("// comment");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Comments, "token is Comments");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "// comment", "full comment");
}

static void test_comment_doc() {
    LexerTest t;
    auto r = t.lex("/// doc");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Docs, "token is Docs");
}

static void test_comment_block() {
    LexerTest t;
    auto r = t.lex("/* block */");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Comments, "token is Comments");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "/* block */", "full comment");
}

static void test_comment_block_doc() {
    LexerTest t;
    auto r = t.lex("/** doc */");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Docs, "token is Docs");
}

static void test_comment_unterminated() {
    LexerTest t;
    auto r = t.lex("/* unterminated");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_keyword_fn() {
    LexerTest t;
    auto r = t.lex("fn");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Fn, "token is Fn");
}

static void test_keyword_all() {
    LexerTest t;
    auto r = t.lex("if else for in match return break continue while struct trait fn let var const "
                   "mut pub mod import use as");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::If, "if is If");
    CHECK_EQ(r.stream.src[1].kind, lexer::TokenKind::If, "else is If");
    CHECK_EQ(r.stream.src[2].kind, lexer::TokenKind::For, "for is For");
    CHECK_EQ(r.stream.src[3].kind, lexer::TokenKind::In, "In");
    CHECK_EQ(r.stream.src[4].kind, lexer::TokenKind::Match, "match is Match");
    CHECK_EQ(r.stream.src[5].kind, lexer::TokenKind::Control, "return is Control");
    CHECK_EQ(r.stream.src[6].kind, lexer::TokenKind::Control, "break is Control");
    CHECK_EQ(r.stream.src[7].kind, lexer::TokenKind::Control, "continue is Control");
    CHECK_EQ(r.stream.src[8].kind, lexer::TokenKind::Control, "while is Control");
    CHECK_EQ(r.stream.src[9].kind, lexer::TokenKind::Struct, "struct is Struct");
    CHECK_EQ(r.stream.src[10].kind, lexer::TokenKind::Trait, "trait is Trait");
    CHECK_EQ(r.stream.src[11].kind, lexer::TokenKind::Fn, "fn is Fn");
    CHECK_EQ(r.stream.src[12].kind, lexer::TokenKind::Variable, "let is Variable");
    CHECK_EQ(r.stream.src[13].kind, lexer::TokenKind::Variable, "var is Variable");
    CHECK_EQ(r.stream.src[14].kind, lexer::TokenKind::Variable, "const is Variable");
    CHECK_EQ(r.stream.src[15].kind, lexer::TokenKind::Mutable, "mut is Mutable");
    CHECK_EQ(r.stream.src[16].kind, lexer::TokenKind::Visibility, "pub is Visibility");
    CHECK_EQ(r.stream.src[17].kind, lexer::TokenKind::Visibility, "mod is Visibility");
    CHECK_EQ(r.stream.src[18].kind, lexer::TokenKind::Module, "import is Module");
    CHECK_EQ(r.stream.src[19].kind, lexer::TokenKind::Using, "use is Using");
    CHECK_EQ(r.stream.src[20].kind, lexer::TokenKind::As, "as is As");
}

static void test_identifier_simple() {
    LexerTest t;
    auto r = t.lex("foo");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Identifier, "token is Identifier");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "foo", "lexeme is 'foo'");
}

static void test_identifier_underscore() {
    LexerTest t;
    auto r = t.lex("_foo123");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Identifier, "token is Identifier");
    CHECK_EQ(r.stream.lexeme(r.stream.src[0]), "_foo123", "lexeme is '_foo123'");
}

static void test_operators() {
    LexerTest t;
    auto r = t.lex("+ - * / = < > ! & |");
    CHECK(r.ok, "lex succeeds");
    for (size_t i = 0; i < 10; i++) {
        CHECK_EQ(r.stream.src[i].kind, lexer::TokenKind::Operators, "token is Operators");
    }
}

static void test_punctuation() {
    LexerTest t;
    auto r = t.lex("( ) { } [ ] : ; , .");
    CHECK(r.ok, "lex succeeds");
    for (size_t i = 0; i < 10; i++) {
        CHECK_EQ(r.stream.src[i].kind, lexer::TokenKind::Punctuation, "token is Punctuation");
    }
}

static void test_empty_input() {
    LexerTest t;
    auto r = t.lex("");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.len, 1u, "1 token (End only)");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::End, "token is End");
}

static void test_single_char_invalid() {
    LexerTest t;
    auto r = t.lex("$");
    CHECK(!r.ok, "lex fails");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_multiple_tokens() {
    LexerTest t;
    auto r = t.lex("let x = 42");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.src[0].kind, lexer::TokenKind::Variable, "let is Variable");
    CHECK_EQ(r.stream.src[1].kind, lexer::TokenKind::Identifier, "x is Identifier");
    CHECK_EQ(r.stream.src[2].kind, lexer::TokenKind::Operators, "= is Operators");
    CHECK_EQ(r.stream.src[3].kind, lexer::TokenKind::LitVal, "42 is LitVal");
}

static void test_whitespace_only() {
    LexerTest t;
    auto r = t.lex("   \t\n  ");
    CHECK(r.ok, "lex succeeds");
    CHECK_EQ(r.stream.len, 1u, "1 token (End only)");
}

static void test_lexer() {
    test_decimal_42();
    test_decimal_0();
    test_float_314();
    test_float_dot_only();
    test_hex_ff();
    test_hex_upper();
    test_hex_no_digits();
    test_hex_invalid_digit();
    test_octal_77();
    test_octal_no_digits();
    test_octal_invalid_digit();
    test_octal_invalid_after_valid();
    test_binary_1010();
    test_binary_no_digits();
    test_binary_invalid_digit();
    test_string_simple();
    test_string_empty();
    test_string_escape_n();
    test_string_escape_invalid();
    test_string_unterminated();
    test_string_backslash_at_eof();
    test_char_literal();
    test_comment_line();
    test_comment_doc();
    test_comment_block();
    test_comment_block_doc();
    test_comment_unterminated();
    test_keyword_fn();
    test_keyword_all();
    test_identifier_simple();
    test_identifier_underscore();
    test_operators();
    test_punctuation();
    test_empty_input();
    test_single_char_invalid();
    test_multiple_tokens();
    test_whitespace_only();
}

TEST_MAIN(lexer)
