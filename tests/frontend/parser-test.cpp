#include "frontend/parser/actions.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"
#include "common/parser/builder.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace hooks::parser {

using Parser = generated_parser::Parser<sample::ParseOutput>;
using Token = generated_lexer::Token;

Recovery recover(Parser &parser, const Token &token) {
    if (parser.diagnostics().size() == 1 &&
        parser.diagnostics()[0].message == "mark")
        return generated_parser::Recovery::Skip;
    if (token.kind == generated_lexer::TokenKind::Unknown ||
        (token.kind == generated_lexer::TokenKind::Punctuation &&
         token.punc == ')'))
        return generated_parser::Recovery::Abort;
    if (token.kind == generated_lexer::TokenKind::Punctuation &&
        token.punc == ']') {
        parser.diag({0, 1}, "aborted by recover");
        parser.abort();
        return generated_parser::Recovery::Skip;
    }
    return generated_parser::Recovery::Skip;
}

void beginModule(Parser &parser, const Token &token) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    parser.output().count += 1;
    (void)token;
}

void sawTopIdentifier(Parser &parser, const Token &token) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    ++parser.output().count;
    (void)token;
}

void sawSemi(Parser &, const Token &) {}

void enterType(Parser &parser, const Token &token) {
    if (!parser.hasOutput())
        parser.setOutput(sample::ParseOutput{});
    (void)token;
}

void moduleName(Parser &, const Token &) {}

void globalName(Parser &, const Token &) {}

void leafExpr(Parser &, const Token &) {}

void binaryExpr(Parser &, const Token &) {}

void unaryExpr(Parser &, const Token &) {}

void typeName(Parser &, const Token &) {}

} // namespace hooks::parser

namespace {

using generated_parser::Parser;
using generated_parser::ParserState;
using generated_lexer::Lexer;
using generated_lexer::TokenStream;
using generated_lexer::TokenKind;
using common::memory::Arena;
using common::memory::StringInterner;

} // namespace

namespace sample {

struct TreeOutput {
    int id = 0;
    int childCount = 0;
};

void attach(TreeOutput &parent, TreeOutput child) {
    (void)child;
    ++parent.childCount;
}

} // namespace sample

namespace {

bool expect_output_builder() {
    common::memory::Arena arena;
    common::parser::OutputBuilder<sample::TreeOutput> builder(arena);
    if (!builder.empty() || builder.size() != 0)
        return false;

    builder.push(sample::TreeOutput{1, 0});
    builder.push(sample::TreeOutput{2, 0});
    if (builder.size() != 2 || builder.top().id != 2)
        return false;

    builder.attach();
    if (builder.size() != 1 || builder.top().childCount != 1)
        return false;

    builder.push(sample::TreeOutput{3, 0});
    builder.push(sample::TreeOutput{4, 0});
    builder.push(sample::TreeOutput{5, 0});
    builder.close(2);
    if (builder.size() != 2 || builder.top().id != 3 ||
        builder.top().childCount != 1)
        return false;

    const sample::TreeOutput root = builder.pop();
    return root.id == 3 && builder.size() == 1 && builder.pop().id == 1;
}

bool expect_parse(const char *source, bool ok) {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run(source, strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, source);
    if (result.isOk() != ok) {
        std::cerr << "parse result mismatch for '" << source << "'\n";
        return false;
    }
    return true;
}

bool expect_helpers() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("alpha + 1", strings);
    Parser<sample::ParseOutput> parser(arena);

    parser.reset(tokens, "alpha + 1");
    bool ok = parser.tokenStream().size() == 4 && parser.tokenStream().empty() == false;
    ok &= parser.current().kind == TokenKind::Identifier;
    ok &= parser.peek().kind == TokenKind::Identifier;
    ok &= parser.tokenStream().hasNext();
    ok &= parser.lexeme(parser.current()) == "alpha";
    ok &= parser.span_slice(parser.current().span) == "alpha";

    parser.tokenStream().advance();
    ok &= parser.tokenStream().match(TokenKind::Operators);
    ok &= parser.tokenStream().matchAll(TokenKind::LitVal, TokenKind::End);
    ok &= !parser.tokenStream().hasNext();
    parser.tokenStream().reset();
    ok &= parser.tokenStream().matchAllLexeme("alpha", "+", "1", "");

    parser.reset(tokens, "alpha + 1");
    const auto tokensAround = parser.slice(0, 2);
    ok &= tokensAround.size() == 2 && tokensAround[0].kind == TokenKind::Identifier;
    ok &= parser.lookupState("Module") == ParserState::Module;
    ok &= parser.lookupState("Nope") == ParserState::TopLevel;

    parser.pushState(ParserState::Module);
    ok &= parser.topState() == ParserState::Module;
    parser.popState();
    ok &= parser.topState() == ParserState::TopLevel;

    parser.pushState(ParserState::Type);
    ok &= parser.popState(ParserState::Type);
    ok &= parser.topState() == ParserState::TopLevel;
    ok &= !parser.popState(ParserState::TopLevel);

    parser.diag({0, 1}, "boom");
    ok &= parser.error();
    ok &= parser.diagnostics().size() == 1;
    ok &= parser.diagnostics()[0].message == "boom";
    return ok;
}

bool expect_top_level_and_pop() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    {
        TokenStream tokens = lexer.run("fn", strings);
        Parser<sample::ParseOutput> parser(arena);
        const auto result = parser.parse(tokens, "fn");
        if (!result.isOk() || parser.output().count != 1)
            return false;
    }

    Arena badArena;
    StringInterner badStrings(badArena);
    TokenStream badTokens = lexer.run(")", badStrings);
    Parser<sample::ParseOutput> badParser(badArena);
    const auto bad = badParser.parse(badTokens, ")");
    return bad.isError();
}

bool expect_parent_chain() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("module alpha ;", strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, "module alpha ;");
    if (!result.isOk())
        return false;
    if (parser.stack().size() != 1 || parser.topState() != ParserState::TopLevel)
        return false;
    return parser.stack().size() == 1 &&
           parser.topState() == ParserState::TopLevel;
}

bool expect_parent_chain_deep() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("fn def [ * ] ;", strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, "fn def [ * ] ;");
    if (!result.isOk())
        return false;
    return parser.stack().size() == 1 &&
           parser.topState() == ParserState::TopLevel;
}

bool expect_global_end() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("module alpha", strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, "module alpha");
    if (!result.isOk())
        return false;
    if (parser.output().sawEnd)
        return false;
    return true;
}

bool expect_recovery_abort() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run(")", strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, ")");
    return result.isError();
}

bool expect_recovery_skip() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("+", strings);
    Parser<sample::ParseOutput> parser(arena);
    parser.diag({0, 1}, "mark");
    const auto result = parser.parse(tokens, "+");
    return result.isOk();
}

bool expect_recovery_abort_override() {
    Arena arena;
    StringInterner strings(arena);
    Lexer lexer;
    TokenStream tokens = lexer.run("]", strings);
    Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, "]");
    return result.isError();
}

bool expect_allow() {
    {
        Arena arena;
        StringInterner strings(arena);
        Lexer lexer;
        // Identifier at top level is accepted, so "x ;" should be accepted and
        // consume both tokens.
        TokenStream tokens = lexer.run("x ;", strings);
        Parser<sample::ParseOutput> parser(arena);
        const auto result = parser.parse(tokens, "x ;");
        if (!result.isOk() || parser.output().count != 1)
            return false;
    }

    {
        Arena badArena;
        StringInterner badStrings(badArena);
        Lexer badLexer;
        TokenStream badTokens = badLexer.run(")", badStrings);
        Parser<sample::ParseOutput> badParser(badArena);
        const auto bad = badParser.parse(badTokens, ")");
        if (!bad.isError())
            return false;
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= expect_parse("alpha", true);
    ok &= expect_parse("x ;", true);
    ok &= expect_helpers();
    ok &= expect_output_builder();
    ok &= expect_top_level_and_pop();
    ok &= expect_parent_chain();
    ok &= expect_parent_chain_deep();
    ok &= expect_global_end();
    ok &= expect_recovery_abort();
    ok &= expect_recovery_skip();
    ok &= expect_recovery_abort_override();
    ok &= expect_allow();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
