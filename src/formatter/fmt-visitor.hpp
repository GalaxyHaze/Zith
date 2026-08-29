#pragma once

#include "frontend/frontend.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace zith::formatter {

class FmtVisitor {
    const frontend::FrontendSnapshot &snapshot_;
    std::string out_;
    int indent_      = 0;
    bool line_start_ = true;

public:
    explicit FmtVisitor(const frontend::FrontendSnapshot &snapshot);

    void format();
    [[nodiscard]] const std::string &result() const noexcept {
        return out_;
    }

private:
    [[nodiscard]] std::string_view sourceText(frontend::TextSpan span) const noexcept;
    void emitOriginal(frontend::TextSpan span);

    void visitDecl(frontend::DeclId id);
    void visitStmt(frontend::StmtId id);
    void visitExpr(frontend::ExprId id, int parent_prec = -1);
    void emitType(frontend::TypeExprId id);

    [[nodiscard]] const frontend::Declaration *declaration(frontend::DeclId id) const noexcept;
    [[nodiscard]] const frontend::Statement *statement(frontend::StmtId id) const noexcept;
    [[nodiscard]] const frontend::Expression *expression(frontend::ExprId id) const noexcept;
    [[nodiscard]] const frontend::TypeExpression *typeExpr(frontend::TypeExprId id) const noexcept;

    [[nodiscard]] std::size_t firstTokenIndex(frontend::TextSpan span) const noexcept;
    [[nodiscard]] std::size_t prefixStartTokenIndex(std::size_t first_token) const noexcept;
    [[nodiscard]] std::size_t findTokenIndex(frontend::TextSpan span,
                                             std::string_view text) const noexcept;
    [[nodiscard]] std::size_t firstElseTokenIndex(frontend::TextSpan opening_span) const noexcept;
    [[nodiscard]] bool containsComment(frontend::TextSpan span) const noexcept;
    [[nodiscard]] bool hasHorizontalGap(std::size_t left_token,
                                        std::size_t right_token) const noexcept;
    [[nodiscard]] std::string_view tokenText(std::size_t token_index) const noexcept;
    [[nodiscard]] int exprPrecedence(const frontend::Expression &expr) const noexcept;
    [[nodiscard]] int binaryPrecedence(std::string_view op) const noexcept;
    [[nodiscard]] bool isStatementExpr(const frontend::Expression &expr) const noexcept;

    void emit(std::string_view text);
    void appendRaw(std::string_view text);
    void newline();
    void blankLine();
    void emitLeadingComments(std::size_t token_index);
    void emitDeclPrefix(frontend::TextSpan span);

    void emitImportDecl(const frontend::Declaration &decl);
    void emitFunctionDecl(const frontend::Declaration &decl);
    void emitVariableDecl(const frontend::Declaration &decl);
    void emitNominalDecl(const frontend::Declaration &decl);
    void emitTraitOrInterfaceDecl(const frontend::Declaration &decl);
    void emitInterfaceFields(const frontend::Declaration &decl);
};

} // namespace zith::formatter
