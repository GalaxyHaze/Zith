#include "frontend/frontend-printer.hpp"

#include <cstdio>
#include <string_view>

namespace zith::frontend {

namespace {

[[nodiscard]] const char *tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::Identifier:
        return "Identifier";
    case TokenKind::Keyword:
        return "Keyword";
    case TokenKind::Literal:
        return "Literal";
    case TokenKind::Operator:
        return "Operator";
    case TokenKind::Punctuation:
        return "Punctuation";
    case TokenKind::Unknown:
        return "Unknown";
    case TokenKind::End:
        return "End";
    }
    return "?";
}

[[nodiscard]] const char *declKindName(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
        return "Function";
    case DeclKind::TypeAlias:
        return "TypeAlias";
    case DeclKind::Struct:
        return "Struct";
    case DeclKind::Enum:
        return "Enum";
    case DeclKind::Union:
        return "Union";
    case DeclKind::Trait:
        return "Trait";
    case DeclKind::Interface:
        return "Interface";
    case DeclKind::Variable:
        return "Variable";
    case DeclKind::Context:
        return "Context";
    case DeclKind::Word:
        return "Word";
    case DeclKind::Import:
        return "Import";
    case DeclKind::Macro:
        return "Macro";
    case DeclKind::Error:
        return "Error";
    }
    return "?";
}

[[nodiscard]] const char *exprKindName(ExprKind kind) {
    switch (kind) {
    case ExprKind::Name:
        return "Name";
    case ExprKind::Literal:
        return "Literal";
    case ExprKind::Unary:
        return "Unary";
    case ExprKind::Binary:
        return "Binary";
    case ExprKind::DockCall:
        return "DockCall";
    case ExprKind::Call:
        return "Call";
    case ExprKind::Block:
        return "Block";
    case ExprKind::If:
        return "If";
    case ExprKind::While:
        return "While";
    case ExprKind::For:
        return "For";
    case ExprKind::ForIn:
        return "ForIn";
    case ExprKind::Return:
        return "Return";
    case ExprKind::Assign:
        return "Assign";
    case ExprKind::OptionalProp:
        return "OptionalProp";
    case ExprKind::Index:
        return "Index";
    case ExprKind::SliceRange:
        return "SliceRange";
    case ExprKind::PackLiteral:
        return "PackLiteral";
    case ExprKind::Field:
        return "Field";
    case ExprKind::Arrow:
        return "Arrow";
    case ExprKind::StructLiteral:
        return "StructLiteral";
    case ExprKind::ArrayLiteral:
        return "ArrayLiteral";
    case ExprKind::When:
        return "When";
    case ExprKind::Range:
        return "Range";
    case ExprKind::Cast:
        return "Cast";
    case ExprKind::IsNull:
        return "IsNull";
    case ExprKind::IsType:
        return "IsType";
    case ExprKind::Placeholder:
        return "Placeholder";
    case ExprKind::LayoutIntrinsic:
        return "LayoutIntrinsic";
    case ExprKind::OwnershipCoerce:
        return "OwnershipCoerce";
    case ExprKind::MacroCall:
        return "MacroCall";
    case ExprKind::WhenGuard:
        return "WhenGuard";
    case ExprKind::Error:
        return "Error";
    }
    return "?";
}

[[nodiscard]] const char *visibilityName(Visibility v) {
    switch (v) {
    case Visibility::Private:
        return "private";
    case Visibility::Public:
        return "public";
    case Visibility::Module:
        return "module";
    }
    return "?";
}

void printIndent(int depth) {
    for (int i = 0; i < depth; ++i)
        std::fputs("  ", stdout);
}

void printExpression(ExprId id, const std::vector<Expression> &expressions,
                     const std::vector<Statement> &statements, int depth) {
    if (!id)
        return;
    const auto index = id.value - 1;
    if (index >= expressions.size())
        return;
    const auto &expr = expressions[index];
    printIndent(depth);
    std::printf("Expression %s", exprKindName(expr.kind));
    if (!expr.text.empty())
        std::printf(" '%s'", expr.text.c_str());
    if (expr.kind == ExprKind::StructLiteral) {
        std::printf(" fields=[");
        for (size_t i = 0; i < expr.field_names.size(); ++i) {
            if (i > 0)
                std::fputs(", ", stdout);
            std::fputs(expr.field_names[i].c_str(), stdout);
        }
        std::fputs("]", stdout);
    }
    std::printf(" [%u..%u]\n", expr.span.start, expr.span.end);

    for (const auto &operand : expr.operands)
        printExpression(operand, expressions, statements, depth + 1);

    for (const auto &stmtId : expr.statements) {
        const auto si = stmtId.value - 1;
        if (si >= statements.size())
            continue;
        const auto &stmt = statements[si];
        printIndent(depth + 1);
        std::printf("Statement ");
        switch (stmt.kind) {
        case StmtKind::Expression:
            std::fputs("Expression", stdout);
            break;
        case StmtKind::Binding:
            std::printf("Binding '%s'", stmt.binding.name.c_str());
            break;
        case StmtKind::Declaration:
            std::printf("Declaration '%s'", stmt.binding.name.c_str());
            break;
        case StmtKind::Return:
            std::fputs("Return", stdout);
            break;
        case StmtKind::Defer:
            std::fputs("Defer", stdout);
            break;
        case StmtKind::Break:
            if (stmt.label.empty())
                std::fputs("Break", stdout);
            else
                std::printf("Break '%s'", stmt.label.c_str());
            break;
        case StmtKind::Continue:
            if (stmt.label.empty())
                std::fputs("Continue", stdout);
            else
                std::printf("Continue '%s'", stmt.label.c_str());
            break;
        case StmtKind::Jump:
            std::printf("Jump '%s' args=%zu", stmt.label.c_str(), stmt.arguments.size());
            break;
        case StmtKind::Error:
            std::fputs("Error", stdout);
            break;
        }
        std::printf(" [%u..%u]\n", stmt.span.start, stmt.span.end);
        printExpression(stmt.expression, expressions, statements, depth + 2);
    }
}

} // namespace

void printTokens(const FrontendSnapshot &snapshot) {
    std::fputs("--- Tokens ---\n", stdout);
    const auto &tokens = snapshot.tokens();
    const auto &source = snapshot.source();
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto &tok = tokens[i];
        // Skip End token — same behavior as legacy printer
        if (tok.kind == TokenKind::End)
            continue;
        const auto lexeme = std::string_view(source).substr(tok.span.start, tok.span.size());
        std::printf("  %-16s \"%.*s\"  [%u..%u]\n", tokenKindName(tok.kind),
                    static_cast<int>(lexeme.size()), lexeme.data(), tok.span.start, tok.span.end);
    }
    std::fputs("---\n", stdout);
}

void printDeclarations(const FrontendSnapshot &snapshot) {
    const auto &decls       = snapshot.declarations();
    const auto &expressions = snapshot.expressions();
    const auto &statements  = snapshot.statements();
    const auto &type_exprs  = snapshot.typeExpressions();

    for (const auto &decl : decls) {
        if (decl.kind == DeclKind::Error && decl.name.empty())
            continue;

        std::printf("Declaration %s '%s' (%s)", declKindName(decl.kind), decl.name.c_str(),
                    visibilityName(decl.visibility));
        if (!decl.ownerName.empty())
            std::printf(" owner='%s'", decl.ownerName.c_str());
        if (!decl.traitName.empty())
            std::printf(" trait='%s'", decl.traitName.c_str());
        if (!decl.externalSymbol.empty())
            std::printf(" external='%s'", decl.externalSymbol.c_str());
        if (decl.kind == DeclKind::Import) {
            std::printf(" path='%s'", decl.import.rawPath.c_str());
            if (!decl.import.alias.empty())
                std::printf(" as='%s'", decl.import.alias.c_str());
        }
        if (decl.kind == DeclKind::Function && decl.functionKind == frontend::FunctionKind::State) {
            std::fputs(" kind=state", stdout);
        }
        std::printf(" [%u..%u]\n", decl.span.start, decl.span.end);

        // Generic parameters
        for (const auto &param : decl.genericParams) {
            std::printf("    GenericParam '%s' [%u..%u]\n", param.name.c_str(), param.span.start,
                        param.span.end);
        }

        // Parameters
        for (const auto &param : decl.parameters) {
            std::printf("    Parameter '%s'", param.name.c_str());
            if (param.type) {
                const auto ti = param.type.value - 1;
                if (ti < type_exprs.size())
                    std::printf(" : %s", type_exprs[ti].name.c_str());
            }
            std::printf(" [%u..%u]\n", param.span.start, param.span.end);
        }

        // Nested members are stored as separate declarations for traits, so make
        // them visible as children of the enum-like owner instead of only as
        // standalone declarations at module depth.
        if (decl.kind == DeclKind::Trait) {
            for (const auto &member : decls) {
                if (member.kind != DeclKind::Function || member.ownerName != decl.name)
                    continue;
                if (member.span.start < decl.span.start || member.span.end > decl.span.end)
                    continue;
                std::printf("    Method '%s' owner='%s'", member.name.c_str(),
                            member.ownerName.c_str());
                for (const auto &param : member.parameters)
                    std::printf(" %s", param.name.c_str());
                if (member.body)
                    std::printf(" (default body)");
                std::printf(" [%u..%u]\n", member.span.start, member.span.end);
            }
        } else if (decl.kind == DeclKind::Interface) {
            std::printf("    Fields: ");
            for (size_t index = 0; index < decl.parameters.size(); ++index) {
                if (index != 0U)
                    std::fputs(", ", stdout);
                std::fputs(decl.parameters[index].name.c_str(), stdout);
            }
            std::fputs("\n", stdout);
            for (const auto &member : decls) {
                if (member.kind != DeclKind::Function || member.ownerName != decl.name)
                    continue;
                if (member.span.start < decl.span.start || member.span.end > decl.span.end)
                    continue;
                std::printf("    Method '%s' owner='%s'", member.name.c_str(),
                            member.ownerName.c_str());
                for (const auto &param : member.parameters)
                    std::printf(" %s", param.name.c_str());
                if (member.body)
                    std::printf(" (default body)");
                std::printf(" [%u..%u]\n", member.span.start, member.span.end);
            }
        }

        // Declared type
        if (decl.declaredType) {
            const auto ti = decl.declaredType.value - 1;
            if (ti < type_exprs.size())
                std::printf("    Type: %s [%u..%u]\n", type_exprs[ti].name.c_str(),
                            type_exprs[ti].span.start, type_exprs[ti].span.end);
        }

        // Initializer (Variable)
        if (decl.initializer)
            printExpression(decl.initializer, expressions, statements, 1);

        // Body (Function)
        if (decl.body)
            printExpression(decl.body, expressions, statements, 1);
    }
}

} // namespace zith::frontend
