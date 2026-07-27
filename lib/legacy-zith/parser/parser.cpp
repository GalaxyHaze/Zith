#include "parser.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/recovery.hpp"
#include "legacy-zith/parser/scan-helpers.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zith::parser {

using ast::kInvalidDecl;
using ast::kInvalidExpr;
// using ast::kInvalidStmt;
using diagnostics::Severity;
using lexer::TokenKind;
using namespace diagnostics::err;

// ── non-consuming checks ────────────────────────────────────────────────

bool Parser::check(lexer::TokenKind kind) const {
    return !eof() && peek().kind == kind;
}

bool Parser::check(char c) const {
    return !eof() && peek().punc == c;
}

bool Parser::checkAny(std::initializer_list<lexer::TokenKind> kinds) const {
    if (eof())
        return false;
    for (auto k : kinds)
        if (peek().kind == k)
            return true;
    return false;
}

const lexer::Token &Parser::previous() const {
    return tok->src[tok->offset - 1];
}

bool Parser::isAtStartOfStmt() const {
    if (eof())
        return false;
    switch (peek().kind) {
    case lexer::TokenKind::Variable:
    case lexer::TokenKind::Control:
    case lexer::TokenKind::If:
    case lexer::TokenKind::For:
    case lexer::TokenKind::Using:
    case lexer::TokenKind::Identifier:
    case lexer::TokenKind::LitVal:
    case lexer::TokenKind::Punctuation:
    case lexer::TokenKind::End:
        return true;
    default:
        return false;
    }
}

void Parser::errorHere(std::string_view msg) {
    diag->report(Severity::Error, diagnostics::err::ExpectedExpr, std::string(msg), peek().span);
}

void Parser::errorHere(std::string_view msg, diagnostics::ErrCode code) {
    diag->report(Severity::Error, code, std::string(msg), peek().span);
}

void Parser::errorExpected(std::string_view expected) {
    std::string msg = "expected ";
    msg += expected;
    msg += " but got ";
    msg += lexer::tokenKindName(peek().kind);
    diag->report(Severity::Error, diagnostics::err::ExpectedExpr, std::move(msg), peek().span);
}

void Parser::errorExpected(std::string_view expected, diagnostics::ErrCode code) {
    std::string msg = "expected ";
    msg += expected;
    msg += " but got ";
    msg += lexer::tokenKindName(peek().kind);
    diag->report(Severity::Error, code, std::move(msg), peek().span);
}

// ── token helpers ──────────────────────────────────────────────────────

std::string_view Parser::lexeme() const {
    return tok->lexeme();
}

const lexer::Token &Parser::peek() const {
    return tok->peek();
}

const lexer::Token &Parser::peek(uint32_t n) const {
    return tok->peek(n);
}

void Parser::advance() {
    tok->advance();
}

void Parser::advance(uint32_t n) {
    tok->advance(n);
}

bool Parser::eof() const {
    return tok->is_empty();
}

bool Parser::match(TokenKind kind) {
    if (!eof() && peek().is(kind)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(const std::string_view &kind) {
    if (!eof() && lexeme() == kind) {
        advance();
        return true;
    }
    return false;
}

bool Parser::expect(TokenKind kind) {
    if (tok->peek().is(kind)) {
        advance();
        return true;
    }
    diag->report(Severity::Error, ExpectedExpr,
                 std::string("expected ") + lexer::tokenKindName(kind) + " but got " +
                     lexer::tokenKindName(peek().kind),
                 peek().span);
    return false;
}

bool Parser::expect(const std::string_view &kind) {
    if (lexeme() == kind) {
        advance();
        return true;
    }
    diag->report(Severity::Error, ExpectedExpr,
                 std::string("expected ") + kind.data() + " but got " +
                     lexer::tokenKindName(peek().kind),
                 peek().span);
    return false;
}

bool Parser::consume(char c) {
    if (!eof() && peek().punc == c) {
        advance();
        return true;
    }
    return false;
}

bool Parser::consume(TokenKind kind) {
    if (!eof() && peek().is(kind)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::expectPunc(char c) {
    if (consume(c))
        return true;
    std::string msg = "expected '";
    msg += c;
    msg += "' but got ";
    msg += lexer::tokenKindName(peek().kind);
    errorHere(msg);
    return false;
}

bool Parser::expectIdent(std::string_view &out) {
    if (check(TokenKind::Identifier)) {
        out = lexeme();
        advance();
        return true;
    }
    std::string msg = "expected identifier but got ";
    msg += lexer::tokenKindName(peek().kind);
    errorHere(msg, diagnostics::err::ExpectedIdent);
    return false;
}

std::string_view Parser::expectIdent() {
    if (check(TokenKind::Identifier)) {
        auto name = lexeme();
        advance();
        return name;
    }
    std::string msg = "expected identifier but got ";
    msg += lexer::tokenKindName(peek().kind);
    errorHere(msg, diagnostics::err::ExpectedIdent);
    return {};
}

memory::Span Parser::spanFrom(memory::Span start) const {
    if (tok->offset > 0)
        return {start.file, start.start, previous().span.end};
    return start;
}

memory::Span Parser::spanFrom(ast::ExprId lhs, ast::ExprId rhs) const {
    auto ls = bld->exprSpan(lhs);
    auto rs = (rhs != ast::kInvalidExpr) ? bld->exprSpan(rhs) : ls;
    return {ls.file, ls.start, rs.end};
}

// ── statement parsing ──────────────────────────────────────────────────

ast::StmtId Parser::parseStmt() {
    skipComments(*tok);
    auto ret_span = peek().span;

    if (consume(TokenKind::Using)) {
        auto use_span = previous().span;
        if (!check(TokenKind::Identifier)) {
            errorExpected("context or word name", diagnostics::err::ExpectedIdent);
            return ast::kInvalidStmt;
        }

        auto first_segment = lexeme();
        auto path_start    = peek().span;
        advance();

        bool is_path = false;
        while (consume('.')) {
            is_path = true;
            if (!check(TokenKind::Identifier)) {
                errorExpected("identifier after '.'", diagnostics::err::ExpectedIdent);
                break;
            }
            advance();
        }

        std::string_view target_path;
        std::string_view context_name = first_segment;
        if (is_path) {
            auto path_end = previous().span.end;
            target_path   = {tok->file_base + path_start.start, path_end - path_start.start};
            context_name  = {};
        }

        std::string_view alias_name;
        if (consume(TokenKind::As))
            alias_name = expectIdent();

        ast::ExprId block = ast::kInvalidExpr;
        if (check('{')) {
            advance();
            block = parseBlock();
        } else if (check(';')) {
            advance();
        }

        return bld->useStmt(context_name, memory::DynArray<std::string_view>{bld->arena()},
                            alias_name, target_path, block, spanFrom(use_span));
    }

    // marker <name> { ... }
    if (match("marker")) {
        auto marker_span = peek().span;
        auto name        = lexeme();
        advance();
        if (check('{')) {
            advance();
            memory::DynArray<ast::StmtId> body{bld->arena()};
            while (!eof() && !check('}')) {
                skipComments(*tok);
                if (check('}'))
                    break;
                body.push(parseStmt());
            }
            if (check('}'))
                advance();
            return bld->markerStmt(name, std::move(body), spanFrom(marker_span));
        }
        // If not followed by '{', treat as jump target (label:)
        // fall through to expression parsing
    }

    // jump <target>
    if (match("jump")) {
        auto jump_span = peek().span;
        auto target    = lexeme();
        advance();
        if (check(';'))
            advance();
        return bld->gotoStmt(target, spanFrom(jump_span));
    }

    if (match("return")) {
        auto val = eof() || check('}') ? kInvalidExpr : parseExpr();
        if (!check(';'))
            skipUntil({TokenKind::End, TokenKind::Punctuation});
        else
            advance();
        return bld->retStmt(val, spanFrom(ret_span));
    }

    if (match("break")) {
        if (check(';'))
            advance();
        auto nil = bld->litExpr(ast::LitKind::Nil, "null", peek().span);
        return bld->addStmt(nil, bld->exprSpan(nil));
    }

    if (match("continue")) {
        if (check(';'))
            advance();
        auto nil = bld->litExpr(ast::LitKind::Nil, "null", peek().span);
        return bld->addStmt(nil, bld->exprSpan(nil));
    }

    if (consume(TokenKind::Variable)) {
        auto let_span    = previous().span;
        auto name        = expectIdent();
        auto type_annot  = parseOptTypeAnnotation();
        ast::ExprId init = kInvalidExpr;
        if (consume('='))
            init = parseExpr();
        if (check(';'))
            advance();
        memory::DynArray<std::string_view> names{bld->arena()};
        names.push(name);
        return bld->letStmt(std::move(names), false, type_annot, init, spanFrom(let_span));
    }

    auto expr = parseExpr();
    if (check('=')) {
        auto assign_span = peek().span;
        advance();
        auto rhs = parseExpr();
        if (check(';'))
            advance();
        return bld->assign(expr, rhs, spanFrom(assign_span));
    }
    // Trailing ?/! propagate acts as ; — no semicolon needed
    if (!check(';')) {
        auto &node = bld->getExpr(expr);
        if (auto *unary = std::get_if<ast::UnaryNode>(&node)) {
            if (unary->op == ast::UnaryOp::PropagateOpt ||
                unary->op == ast::UnaryOp::PropagateRes) {
                if (check(';'))
                    advance();
                return bld->addStmt(expr, bld->exprSpan(expr));
            }
        }
    }
    if (check(';'))
        advance();

    return bld->addStmt(expr, bld->exprSpan(expr));
}

// ── declaration parsing ────────────────────────────────────────────────

ast::DeclId Parser::parseFnDecl() {
    if (!check(TokenKind::Identifier)) {
        std::string msg = "expected function name but got ";
        msg += lexer::tokenKindName(peek().kind);
        errorHere(msg, diagnostics::err::ExpectedIdent);
        return kInvalidDecl;
    }
    auto name_span = peek().span;
    auto name      = lexeme();
    advance();

    if (!consume('(')) {
        errorExpected("'(' after function name", diagnostics::err::UnclosedParen);
        return kInvalidDecl;
    }

    auto params = parseDelimited(*tok, bld->arena(), ')', [this]() -> std::string_view {
        if (!check(TokenKind::Identifier)) {
            std::string msg = "expected parameter name but got ";
            msg += lexer::tokenKindName(peek().kind);
            errorHere(msg, diagnostics::err::ExpectedIdent);
            advance();
            return {};
        }
        auto n = lexeme();
        advance();
        return n;
    });
    consume(')');

    ast::ExprId body = kInvalidExpr;
    if (check('{'))
        body = parseBlock();

    return bld->fnDecl(name, std::move(params), body, spanFrom(name_span));
}

ast::DeclId Parser::parseDecl() {
    skipComments(*tok);
    if (check(TokenKind::Fn))
        return parseFnDecl();
    if (check(TokenKind::Struct))
        return kInvalidDecl; // handled during scan
    {
        std::string msg = "expected declaration but got ";
        msg += lexer::tokenKindName(peek().kind);
        errorHere(msg);
    }
    advance();
    return kInvalidDecl;
}

// ── Parser helper implementations ───────────────────────────────────────

ast::TypeExprId Parser::parseOptTypeAnnotation() {
    if (consume(':'))
        return parseTypeExpr();
    return ast::kInvalidTypeExpr;
}

void Parser::skipUntil(std::initializer_list<lexer::TokenKind> sync_tokens) {
    recovery::panic(*tok, sync_tokens);
}

// ── public API (backward compat) ─────────────────────────────────────

ProgramResult parseProgram(lexer::TokenStream tokens, ast::AstBuilder &builder,
                           diagnostics::DiagnosticEngine &diags) {
    memory::Arena arena;
    symbols::SymbolTable syms(arena, &builder.interner());
    Parser p(&tokens, &builder, &diags);
    auto result = scan(p, syms);
    p.expandBodies(result, syms);
    return std::move(p.program);
}

} // namespace zith::parser
