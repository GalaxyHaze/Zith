#include "formatter/fmt-visitor.hpp"

namespace zith::formatter {

namespace {

[[nodiscard]] bool overlaps(const frontend::TextSpan left,
                            const frontend::TextSpan right) noexcept {
    return left.start < right.end && right.start < left.end;
}

} // namespace

FmtVisitor::FmtVisitor(const frontend::FrontendSnapshot &snapshot) : snapshot_(snapshot) {}

std::string_view FmtVisitor::sourceText(const frontend::TextSpan span) const noexcept {
    if (span.end <= span.start || span.end > snapshot_.source().size())
        return {};
    return std::string_view(snapshot_.source()).substr(span.start, span.size());
}

void FmtVisitor::appendRaw(const std::string_view text) {
    out_.append(text);
    if (!text.empty())
        line_start_ = text.back() == '\n';
}

void FmtVisitor::emit(const std::string_view text) {
    if (text.empty())
        return;
    if (line_start_) {
        for (int level = 0; level < indent_; ++level)
            out_ += "    ";
        line_start_ = false;
    }
    appendRaw(text);
}

void FmtVisitor::newline() {
    out_ += '\n';
    line_start_ = true;
}

void FmtVisitor::blankLine() {
    if (!out_.empty() && out_.back() != '\n')
        newline();
    if (out_.empty() || out_.back() != '\n')
        newline();
    newline();
}

const frontend::Declaration *FmtVisitor::declaration(const frontend::DeclId id) const noexcept {
    if (!id || id.value > snapshot_.declarations().size())
        return nullptr;
    return &snapshot_.declarations()[id.value - 1U];
}

const frontend::Statement *FmtVisitor::statement(const frontend::StmtId id) const noexcept {
    if (!id || id.value > snapshot_.statements().size())
        return nullptr;
    return &snapshot_.statements()[id.value - 1U];
}

const frontend::Expression *FmtVisitor::expression(const frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot_.expressions().size())
        return nullptr;
    return &snapshot_.expressions()[id.value - 1U];
}

const frontend::TypeExpression *FmtVisitor::typeExpr(const frontend::TypeExprId id) const noexcept {
    if (!id || id.value > snapshot_.typeExpressions().size())
        return nullptr;
    return &snapshot_.typeExpressions()[id.value - 1U];
}

std::size_t FmtVisitor::firstTokenIndex(const frontend::TextSpan span) const noexcept {
    const auto &tokens = snapshot_.tokens();
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].kind == frontend::TokenKind::End)
            break;
        if (tokens[index].span.start >= span.start && tokens[index].span.end <= span.end)
            return index;
    }
    return tokens.empty() ? 0U : tokens.size() - 1U;
}

bool FmtVisitor::hasHorizontalGap(const std::size_t left_token,
                                  const std::size_t right_token) const noexcept {
    const auto &tokens = snapshot_.tokens();
    if (left_token >= tokens.size() || right_token >= tokens.size() || left_token >= right_token)
        return false;
    const auto between = std::string_view(snapshot_.source())
                             .substr(tokens[left_token].span.end,
                                     tokens[right_token].span.start - tokens[left_token].span.end);
    return between.find('\n') == std::string_view::npos &&
           between.find('\r') == std::string_view::npos;
}

std::size_t FmtVisitor::prefixStartTokenIndex(const std::size_t first_token) const noexcept {
    const auto &tokens = snapshot_.tokens();
    if (first_token >= tokens.size())
        return first_token;

    std::size_t current = first_token;
    while (current > 0U) {
        const std::size_t previous = current - 1U;
        if (tokens[previous].kind != frontend::TokenKind::Keyword ||
            !hasHorizontalGap(previous, current))
            break;
        current = previous;
    }
    return current;
}

std::size_t FmtVisitor::findTokenIndex(const frontend::TextSpan span,
                                       const std::string_view text) const noexcept {
    const auto &tokens = snapshot_.tokens();
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].kind == frontend::TokenKind::End)
            break;
        if (tokens[index].span.start < span.start || tokens[index].span.end > span.end)
            continue;
        if (tokenText(index) == text)
            return index;
    }
    return tokens.size();
}

bool FmtVisitor::containsComment(const frontend::TextSpan span) const noexcept {
    for (const auto &trivia : snapshot_.trivia()) {
        if (trivia.kind == frontend::TriviaKind::Whitespace)
            continue;
        if (overlaps(trivia.span, span))
            return true;
    }
    return false;
}

std::string_view FmtVisitor::tokenText(const std::size_t token_index) const noexcept {
    const auto &tokens = snapshot_.tokens();
    if (token_index >= tokens.size())
        return {};
    return sourceText(tokens[token_index].span);
}

int FmtVisitor::binaryPrecedence(const std::string_view op) const noexcept {
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
        op == "<<=" || op == ">>=" || op == "&.=" || op == "|.=" || op == "^.=")
        return 1;
    if (op == "or")
        return 2;
    if (op == "and")
        return 3;
    if (op == "xor")
        return 4;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
        return 5;
    if (op == "|.")
        return 6;
    if (op == "^.")
        return 7;
    if (op == "&.")
        return 8;
    if (op == "<<" || op == ">>")
        return 9;
    if (op == "+" || op == "-")
        return 10;
    if (op == "*" || op == "/" || op == "%")
        return 11;
    return -1;
}

int FmtVisitor::exprPrecedence(const frontend::Expression &expr) const noexcept {
    switch (expr.kind) {
    case frontend::ExprKind::Assign:
        return 1;
    case frontend::ExprKind::Binary:
        return binaryPrecedence(expr.text);
    case frontend::ExprKind::Unary:
        return 12;
    case frontend::ExprKind::IsNull:
        return 5;
    case frontend::ExprKind::Call:
    case frontend::ExprKind::Index:
    case frontend::ExprKind::OptionalProp:
    case frontend::ExprKind::Cast:
    case frontend::ExprKind::Field:
    case frontend::ExprKind::Arrow:
        return 13;
    default:
        return 14;
    }
}

bool FmtVisitor::isStatementExpr(const frontend::Expression &expr) const noexcept {
    return expr.kind == frontend::ExprKind::Block || expr.kind == frontend::ExprKind::If ||
           expr.kind == frontend::ExprKind::While || expr.kind == frontend::ExprKind::For;
}

void FmtVisitor::emitLeadingComments(const std::size_t token_index) {
    const auto &tokens = snapshot_.tokens();
    if (token_index >= tokens.size())
        return;

    const auto &token    = tokens[token_index];
    bool emitted_comment = false;
    for (uint32_t index = 0; index < token.leadingTriviaCount; ++index) {
        const auto &trivia = snapshot_.trivia()[token.leadingTriviaStart + index];
        if (trivia.kind == frontend::TriviaKind::Whitespace) {
            if (emitted_comment) {
                auto text = sourceText(trivia.span);
                int nl    = 0;
                for (char c : text)
                    if (c == '\n')
                        ++nl;
                if (nl >= 2)
                    blankLine();
            }
            continue;
        }
        emit(sourceText(trivia.span));
        newline();
        emitted_comment = true;
    }
}

void FmtVisitor::emitDeclPrefix(const frontend::TextSpan span) {
    const auto first        = firstTokenIndex(span);
    const auto prefix_start = prefixStartTokenIndex(first);
    emitLeadingComments(prefix_start);
    for (std::size_t index = prefix_start; index < first; ++index) {
        emit(tokenText(index));
        emit(" ");
    }
}

void FmtVisitor::emitOriginal(const frontend::TextSpan span) {
    appendRaw(sourceText(span));
}

void FmtVisitor::emitImportDecl(const frontend::Declaration &decl) {
    emitDeclPrefix(decl.span);
    if (decl.import.isExport)
        emit("export ");
    else if (decl.import.isFrom)
        emit("from ");
    else
        emit("import ");

    if (decl.import.isAsset && !decl.import.rawPath.starts_with("assets"))
        emit("asset ");

    if (!decl.import.rawPath.empty())
        emit(decl.import.rawPath);
    else
        emitOriginal(decl.import.pathSpan);

    if (decl.import.depth != 1) {
        emit("(");
        if (decl.import.depth < 0)
            emit("..");
        else
            emit(std::to_string(decl.import.depth));
        emit(")");
    }

    if (!decl.import.selectors.empty()) {
        emit(" { ");
        for (std::size_t index = 0; index < decl.import.selectors.size(); ++index) {
            if (index != 0U)
                emit(", ");
            emit(decl.import.selectors[index].name);
            if (!decl.import.selectors[index].alias.empty()) {
                emit(" as ");
                emit(decl.import.selectors[index].alias);
            }
        }
        emit(" }");
    }

    if (!decl.import.alias.empty()) {
        emit(" as ");
        emit(decl.import.alias);
    }
}

void FmtVisitor::emitType(const frontend::TypeExprId id) {
    const auto *type = typeExpr(id);
    if (type == nullptr)
        return;

    // Memory qualifiers are a prefix on the type, not a separate node: re-print
    // them here or `zithc fmt` would silently drop them.  The kinds below that
    // fall back to `emitOriginal` already replay the prefix from the source span.
    if (type->kind == frontend::TypeExprKind::Name ||
        type->kind == frontend::TypeExprKind::Pointer ||
        type->kind == frontend::TypeExprKind::Optional) {
        if (type->hasMutKeyword)
            emit("mut ");
        switch (type->ownership) {
        case frontend::OwnershipKind::Unique:
            emit("unique ");
            break;
        case frontend::OwnershipKind::Share:
            emit("share ");
            break;
        case frontend::OwnershipKind::Lend:
            emit("lend ");
            break;
        case frontend::OwnershipKind::View:
            emit("view ");
            break;
        case frontend::OwnershipKind::Belong:
            emit("belong ");
            break;
        case frontend::OwnershipKind::Default:
            break;
        }
    }

    switch (type->kind) {
    case frontend::TypeExprKind::Name:
        emit(type->name);
        break;
    case frontend::TypeExprKind::Pointer:
        emit("*");
        if (!type->arguments.empty())
            emitType(type->arguments.front());
        break;
    case frontend::TypeExprKind::Optional:
        emit("?");
        if (!type->arguments.empty())
            emitType(type->arguments.front());
        break;
    case frontend::TypeExprKind::Opaque:
        emit("raw opaque");
        break;
    case frontend::TypeExprKind::Array:
    case frontend::TypeExprKind::Function:
    case frontend::TypeExprKind::Slice:
    case frontend::TypeExprKind::Error:
        emitOriginal(type->span);
        break;
    }
}

void FmtVisitor::emitFunctionDecl(const frontend::Declaration &decl) {
    if (containsComment(decl.span)) {
        emitDeclPrefix(decl.span);
        emitOriginal(decl.span);
        return;
    }

    emitDeclPrefix(decl.span);
    emit("fn ");
    emit(decl.name);
    if (!decl.genericParams.empty()) {
        emit("<");
        for (std::size_t index = 0; index < decl.genericParams.size(); ++index) {
            if (index != 0U)
                emit(", ");
            emit(decl.genericParams[index].name);
            if (decl.genericParams[index].constraint) {
                emit(": ");
                emitType(decl.genericParams[index].constraint);
            }
        }
        emit(">");
    }
    emit("(");
    for (std::size_t index = 0; index < decl.parameters.size(); ++index) {
        if (index != 0U)
            emit(", ");
        emit(decl.parameters[index].name);
        if (decl.parameters[index].type) {
            emit(": ");
            emitType(decl.parameters[index].type);
        }
    }
    if (decl.isVariadic) {
        if (!decl.parameters.empty())
            emit(", ");
        emit("...");
    }
    emit(")");
    if (decl.declaredType) {
        emit(": ");
        emitType(decl.declaredType);
    }
    if (decl.body) {
        emit(" ");
        visitExpr(decl.body);
    } else {
        emit(";");
    }
}

void FmtVisitor::emitVariableDecl(const frontend::Declaration &decl) {
    if (containsComment(decl.span)) {
        emitDeclPrefix(decl.span);
        emitOriginal(decl.span);
        return;
    }

    emitDeclPrefix(decl.span);
    emit(tokenText(firstTokenIndex(decl.span)));
    emit(" ");
    emit(decl.name);
    if (decl.declaredType) {
        emit(": ");
        emitType(decl.declaredType);
    }
    if (decl.initializer) {
        emit(" = ");
        visitExpr(decl.initializer);
    }
    emit(";");
}

void FmtVisitor::emitNominalDecl(const frontend::Declaration &decl) {
    emitDeclPrefix(decl.span);
    emit(tokenText(firstTokenIndex(decl.span)));
    emit(" ");
    emit(decl.name);
    if (!decl.genericParams.empty()) {
        emit("<");
        for (std::size_t index = 0; index < decl.genericParams.size(); ++index) {
            if (index != 0U)
                emit(", ");
            emit(decl.genericParams[index].name);
            if (decl.genericParams[index].constraint) {
                emit(": ");
                emitType(decl.genericParams[index].constraint);
            }
        }
        emit(">");
    }

    const auto body_start = findTokenIndex(decl.span, "{");
    if (body_start < snapshot_.tokens().size()) {
        emit(" ");
        emitOriginal({snapshot_.tokens()[body_start].span.start, decl.span.end});
        return;
    }

    if (sourceText(decl.span).find(';') != std::string_view::npos)
        emit(";");
}

void FmtVisitor::visitDecl(const frontend::DeclId id) {
    const auto *decl = declaration(id);
    if (decl == nullptr)
        return;

    switch (decl->kind) {
    case frontend::DeclKind::Import:
        emitImportDecl(*decl);
        break;
    case frontend::DeclKind::Function:
        emitFunctionDecl(*decl);
        break;
    case frontend::DeclKind::Variable:
        emitVariableDecl(*decl);
        break;
    case frontend::DeclKind::Struct:
    case frontend::DeclKind::Enum:
    case frontend::DeclKind::Union:
        emitNominalDecl(*decl);
        break;
    case frontend::DeclKind::TypeAlias:
    case frontend::DeclKind::Trait:
    case frontend::DeclKind::Interface:
    case frontend::DeclKind::Context:
    case frontend::DeclKind::Word:
    case frontend::DeclKind::Macro:
    case frontend::DeclKind::Error:
        emitDeclPrefix(decl->span);
        emitOriginal(decl->span);
        break;
    }
}

void FmtVisitor::visitStmt(const frontend::StmtId id) {
    const auto *stmt = statement(id);
    if (stmt == nullptr)
        return;

    const auto first = firstTokenIndex(stmt->span);
    emitLeadingComments(first);

    if (containsComment(stmt->span)) {
        emitOriginal(stmt->span);
        return;
    }

    switch (stmt->kind) {
    case frontend::StmtKind::Binding:
        emit(tokenText(first));
        emit(" ");
        emit(stmt->binding.name);
        if (stmt->binding.type) {
            emit(": ");
            emitType(stmt->binding.type);
        }
        if (stmt->binding.initializer) {
            emit(" = ");
            visitExpr(stmt->binding.initializer);
        }
        emit(";");
        break;
    case frontend::StmtKind::Return:
        emit("return");
        if (stmt->expression) {
            emit(" ");
            visitExpr(stmt->expression);
        }
        emit(";");
        break;
    case frontend::StmtKind::Break:
        emit("break;");
        break;
    case frontend::StmtKind::Continue:
        emit("continue;");
        break;
    case frontend::StmtKind::Marker:
        emit("marker ");
        emit(stmt->label);
        if (stmt->expression) {
            emit(" ");
            visitExpr(stmt->expression);
        }
        break;
    case frontend::StmtKind::Jump:
        emit("jump ");
        emit(stmt->label);
        emit(";");
        break;
    case frontend::StmtKind::Expression:
        if (!stmt->expression) {
            emitOriginal(stmt->span);
            break;
        }
        if (const auto *expr = expression(stmt->expression); expr != nullptr) {
            visitExpr(stmt->expression);
            if (!isStatementExpr(*expr))
                emit(";");
        } else {
            emitOriginal(stmt->span);
        }
        break;
    case frontend::StmtKind::Error:
        emitOriginal(stmt->span);
        break;
    }
}

void FmtVisitor::visitExpr(const frontend::ExprId id, const int parent_prec) {
    const auto *expr = expression(id);
    if (expr == nullptr)
        return;

    if (containsComment(expr->span) && expr->kind != frontend::ExprKind::Name &&
        expr->kind != frontend::ExprKind::Literal) {
        emitOriginal(expr->span);
        return;
    }

    const int current_prec = exprPrecedence(*expr);
    // Call, Field and Arrow are left-associative postfix forms: as the base of
    // another postfix expression they never need parentheses (`c.bump(3)`, not
    // `(c.bump)(3)`). Cast still does, since `x as T` binds looser than `.`.
    const bool needs_paren = parent_prec >= 0 && current_prec >= 0 && current_prec <= parent_prec &&
                             expr->kind != frontend::ExprKind::Call &&
                             expr->kind != frontend::ExprKind::Field &&
                             expr->kind != frontend::ExprKind::Arrow;
    if (needs_paren)
        emit("(");

    switch (expr->kind) {
    case frontend::ExprKind::Name:
    case frontend::ExprKind::Literal:
        emit(expr->text);
        break;
    case frontend::ExprKind::Unary:
        if (expr->text == "not") {
            emit("not ");
        } else {
            emit(expr->text);
        }
        if (!expr->operands.empty())
            visitExpr(expr->operands.front(), current_prec);
        break;
    case frontend::ExprKind::Binary:
    case frontend::ExprKind::Assign: {
        if (expr->operands.size() != 2U) {
            emitOriginal(expr->span);
            break;
        }
        // A compound assignment is stored desugared as `x = x op v`, so printing its rhs
        // verbatim would emit `x += x op v`. Re-print only `v`, recovering the source form.
        frontend::ExprId value = expr->operands[1];
        if (expr->kind == frontend::ExprKind::Assign && expr->text != "=") {
            if (const auto *folded = expression(value);
                folded != nullptr && folded->kind == frontend::ExprKind::Binary &&
                folded->operands.size() == 2U) {
                value = folded->operands[1];
            }
        }
        visitExpr(expr->operands[0], current_prec);
        emit(" ");
        emit(expr->text);
        emit(" ");
        visitExpr(value, current_prec);
        break;
    }
    case frontend::ExprKind::Call:
        if (expr->operands.empty()) {
            emitOriginal(expr->span);
            break;
        }
        visitExpr(expr->operands[0], current_prec);
        emit("(");
        for (std::size_t index = 1; index < expr->operands.size(); ++index) {
            if (index != 1U)
                emit(", ");
            visitExpr(expr->operands[index]);
        }
        emit(")");
        break;
    case frontend::ExprKind::Block:
        if (expr->statements.empty()) {
            emit("{}");
            break;
        }
        emit("{");
        newline();
        ++indent_;
        for (const auto stmt_id : expr->statements) {
            visitStmt(stmt_id);
            newline();
        }
        --indent_;
        emit("}");
        break;
    case frontend::ExprKind::If:
        if (expr->operands.size() < 2U) {
            emitOriginal(expr->span);
            break;
        }
        emit("if (");
        visitExpr(expr->operands[0]);
        emit(") ");
        visitExpr(expr->operands[1]);
        if (expr->operands.size() > 2U) {
            emit(" else ");
            visitExpr(expr->operands[2]);
        }
        break;
    case frontend::ExprKind::While:
        if (expr->operands.size() < 2U) {
            emitOriginal(expr->span);
            break;
        }
        emit("while (");
        visitExpr(expr->operands[0]);
        emit(") ");
        visitExpr(expr->operands[1]);
        break;
    case frontend::ExprKind::For:
        // init was desugared into a preceding statement, so the head prints as
        // `for (; cond; step)` to round-trip the remaining clauses.
        if (expr->operands.size() < 2U) {
            emitOriginal(expr->span);
            break;
        }
        emit("for (; ");
        visitExpr(expr->operands[0]);
        emit("; ");
        if (expr->operands.size() >= 3U && expr->operands[2])
            visitExpr(expr->operands[2]);
        emit(") ");
        visitExpr(expr->operands[1]);
        break;
    case frontend::ExprKind::Return:
        emit("return");
        if (!expr->operands.empty()) {
            emit(" ");
            visitExpr(expr->operands.front());
        }
        break;
    case frontend::ExprKind::OptionalProp:
        if (!expr->operands.empty())
            visitExpr(expr->operands.front(), current_prec);
        emit("?");
        break;
    case frontend::ExprKind::Index:
        if (expr->operands.size() < 2U) {
            emitOriginal(expr->span);
            break;
        }
        visitExpr(expr->operands[0], current_prec);
        emit("[");
        visitExpr(expr->operands[1]);
        emit("]");
        break;
    case frontend::ExprKind::Field:
        if (!expr->operands.empty()) {
            visitExpr(expr->operands[0], current_prec);
            emit(".");
            emit(expr->text);
        } else {
            emitOriginal(expr->span);
        }
        break;
    case frontend::ExprKind::Arrow:
        if (!expr->operands.empty()) {
            visitExpr(expr->operands[0], current_prec);
            emit("->");
            emit(expr->text);
        } else {
            emitOriginal(expr->span);
        }
        break;
    case frontend::ExprKind::StructLiteral:
        emit(expr->text);
        emit(" { ");
        for (std::size_t i = 0; i < expr->operands.size(); ++i) {
            if (i != 0U)
                emit(", ");
            if (i < expr->field_names.size()) {
                emit(expr->field_names[i]);
                emit(": ");
            }
            visitExpr(expr->operands[i]);
        }
        emit(" }");
        break;
    case frontend::ExprKind::ArrayLiteral:
        emit("[");
        for (std::size_t i = 0; i < expr->operands.size(); ++i) {
            if (i != 0U)
                emit(", ");
            visitExpr(expr->operands[i]);
        }
        emit("]");
        break;
    case frontend::ExprKind::When:
        if (expr->operands.empty()) {
            emitOriginal(expr->span);
            break;
        }
        emit("when (");
        visitExpr(expr->operands[0]);
        emit(") { ");
        for (std::size_t i = 0; i + 1 < expr->operands.size(); ++i) {
            if (i != 0U)
                emit(", ");
            if (i < expr->conditions.size() && expr->conditions[i]) {
                emit("(");
                visitExpr(expr->conditions[i]);
                emit(")");
            } else {
                emit("(_)");
            }
            emit(" ~> ");
            visitExpr(expr->operands[i + 1U]);
        }
        emit(" }");
        break;
    case frontend::ExprKind::Range:
        if (expr->operands.size() < 2U) {
            emitOriginal(expr->span);
            break;
        }
        visitExpr(expr->operands[0]);
        emit("..");
        visitExpr(expr->operands[1]);
        break;
    case frontend::ExprKind::Cast:
        if (expr->operands.empty()) {
            emitOriginal(expr->span);
            break;
        }
        visitExpr(expr->operands[0], current_prec);
        emit(" as ");
        emitType(expr->cast_type);
        break;
    case frontend::ExprKind::IsNull:
        if (expr->operands.empty()) {
            emitOriginal(expr->span);
            break;
        }
        visitExpr(expr->operands[0], current_prec);
        emit(" is null");
        break;
    case frontend::ExprKind::Placeholder:
        emit("_");
        break;
    case frontend::ExprKind::LayoutIntrinsic:
        emitOriginal(expr->span);
        break;
    case frontend::ExprKind::MacroCall:
        emitOriginal(expr->span);
        break;
    case frontend::ExprKind::Error:
        emitOriginal(expr->span);
        break;
    }

    if (needs_paren)
        emit(")");
}

void FmtVisitor::format() {
    const auto &decls = snapshot_.declarations();
    bool emitted_decl = false;
    for (const auto &decl : decls) {
        if (decl.kind == frontend::DeclKind::Error && decl.span.size() == 0U)
            continue;
        // Methods are printed by their owner: a struct-body method is already
        // inside the struct's emitted body, and an implement-block method is
        // re-emitted below under its `implement Owner { ... }` header.
        if (!decl.ownerName.empty())
            continue;
        if (emitted_decl)
            blankLine();
        visitDecl(decl.id);
        emitted_decl = true;

        // After a nominal declaration, emit the implement blocks that carry its
        // methods, so `implement Owner { fn m() {} }` survives formatting.
        if (decl.kind != frontend::DeclKind::Struct && decl.kind != frontend::DeclKind::Enum &&
            decl.kind != frontend::DeclKind::Union && decl.kind != frontend::DeclKind::Trait)
            continue;
        bool opened = false;
        for (const auto &method : decls) {
            if (method.kind != frontend::DeclKind::Function || method.ownerName != decl.name)
                continue;
            // Skip methods written inside the owner's own body: already emitted.
            if (method.span.start >= decl.span.start && method.span.end <= decl.span.end)
                continue;
            if (!opened) {
                blankLine();
                emit("implement ");
                emit(decl.name);
                emit(" {");
                newline();
                ++indent_;
                opened = true;
            } else {
                blankLine();
            }
            emitFunctionDecl(method);
        }
        if (opened) {
            --indent_;
            newline();
            emit("}");
            newline();
        }
    }

    if (!snapshot_.tokens().empty()) {
        const std::size_t eof_index   = snapshot_.tokens().size() - 1U;
        bool emitted_trailing_comment = false;
        const auto &eof               = snapshot_.tokens()[eof_index];
        for (uint32_t index = 0; index < eof.leadingTriviaCount; ++index) {
            const auto &trivia = snapshot_.trivia()[eof.leadingTriviaStart + index];
            if (trivia.kind == frontend::TriviaKind::Whitespace)
                continue;
            if (emitted_decl && !emitted_trailing_comment)
                blankLine();
            emit(sourceText(trivia.span));
            newline();
            emitted_trailing_comment = true;
        }
    }

    if (out_.empty() || out_.back() != '\n')
        newline();
}

} // namespace zith::formatter
