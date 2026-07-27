#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/token.hpp"
#include "legacy-zith/parser/parse-result.hpp"
#include "legacy-zith/parser/scan-result.hpp"
#include "symbols/symbol-table.hpp"

#include <initializer_list>
#include <string_view>

namespace zith::parser {

struct Parser {
    lexer::TokenStream *tok;
    ast::AstBuilder *bld;
    diagnostics::DiagnosticEngine *diag;
    ast::ProgramNode program;
    symbols::SymbolTable *syms = nullptr;

    Parser(lexer::TokenStream *tok_, ast::AstBuilder *bld_, diagnostics::DiagnosticEngine *diag_)
        : tok(tok_), bld(bld_), diag(diag_), program(bld_->arena()) {}

    // ── Token helpers ────────────────────────────────────────────────
    std::string_view lexeme() const;
    const lexer::Token &peek() const;
    const lexer::Token &peek(uint32_t n) const;
    void advance();
    void advance(uint32_t n);
    [[nodiscard]] bool eof() const;
    bool match(lexer::TokenKind kind);
    bool match(const std::string_view &kind);
    bool expect(lexer::TokenKind kind);
    bool expect(const std::string_view &kind);
    bool consume(char c);
    bool consume(lexer::TokenKind kind);
    bool expectPunc(char c);
    bool expectIdent(std::string_view &out);
    std::string_view expectIdent();

    // ── Non-consuming checks (companion to match/consume) ─────────
    [[nodiscard]] bool check(lexer::TokenKind kind) const;
    [[nodiscard]] bool check(char c) const;
    [[nodiscard]] bool checkAny(std::initializer_list<lexer::TokenKind> kinds) const;

    // ── Previous token access ──────────────────────────────────────
    const lexer::Token &previous() const;

    // ── Start-of-construct detection (for error recovery) ─────────
    [[nodiscard]] bool isAtStartOfStmt() const;

    // ── Structured error helpers ─────────────────────────────────
    void errorHere(std::string_view msg);
    void errorHere(std::string_view msg, diagnostics::ErrCode code);
    void errorExpected(std::string_view expected);
    void errorExpected(std::string_view expected, diagnostics::ErrCode code);

    // ── Expression parsing ───────────────────────────────────────────
    ast::ExprId parsePrimary();
    ast::ExprId parsePrefix();
    ast::ExprId parseExpr(int min_prec);
    ast::ExprId parseExpr();
    ast::ExprId parseBlock();
    ast::StmtId parseStmt();
    ast::DeclId parseFnDecl();
    ast::DeclId parseDecl();

    // ── Type expression parsing ─────────────────────────────────────
    ast::TypeExprId parseTypeExpr();
    ast::TypeExprId parseOrExpr();
    ast::TypeExprId parsePrimaryType();

    // ── Helper patterns ──────────────────────────────────────────────

    // Parse `: Type` annotation if present, returns kInvalidTypeExpr otherwise
    ast::TypeExprId parseOptTypeAnnotation();

    // Skip tokens until a sync point is found (error recovery)
    void skipUntil(std::initializer_list<lexer::TokenKind> sync_tokens);

    // ── Body expansion ───────────────────────────────────────────────
    void expandBodies(ScanResult &result, symbols::SymbolTable &syms);

    memory::Span spanFrom(memory::Span start) const;
    memory::Span spanFrom(ast::ExprId lhs, ast::ExprId rhs) const;
};

ProgramResult parseProgram(lexer::TokenStream tokens, ast::AstBuilder &builder,
                           diagnostics::DiagnosticEngine &diags);

ScanResult scan(Parser &parser, symbols::SymbolTable &syms);

} // namespace zith::parser
