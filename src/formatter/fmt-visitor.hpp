#pragma once

#include "ast/ast-builder.hpp"
#include "ast/ast-nodes.hpp"
#include "ast/type-expr.hpp"
#include "memory/span.hpp"

#include <string>

namespace zith::formatter {

class FmtVisitor {
    const ast::AstBuilder &builder_;
    const ast::ProgramNode &program_;
    std::string out_;
    int indent_ = 0;

public:
    FmtVisitor(const ast::AstBuilder &builder, const ast::ProgramNode &program);

    void format();
    const std::string &result() const {
        return out_;
    }

private:
    // Declarations
    void visitDecl(ast::DeclId id);
    void emitFnDecl(const ast::FnDeclNode &node);
    void emitStructDecl(const ast::StructDeclNode &node);
    void emitEnumDecl(const ast::EnumDeclNode &node);
    void emitUnionDecl(const ast::UnionDeclNode &node);
    void emitTraitDecl(const ast::TraitDeclNode &node);
    void emitInterfaceDecl(const ast::InterfaceDeclNode &node);
    void emitComponentDecl(const ast::ComponentDeclNode &node);
    void emitImport(const ast::ImportNode &node);
    void emitTypeAlias(const ast::TypeAliasDeclNode &node);
    void emitGlobalDecl(const ast::GlobalDeclNode &node);
    void emitWordDecl(const ast::WordDeclNode &node);
    void emitContextDecl(const ast::ContextDeclNode &node);

    // Generics
    void emitGenericParams(const memory::DynArray<ast::GenericParam> &params);

    // Statements
    void visitStmt(ast::StmtId id);
    void emitLet(const ast::LetNode &node);
    void emitAssign(const ast::AssignNode &node);
    void emitRet(const ast::RetNode &node);
    void emitUse(const ast::UseNode &node);

    // Expressions
    void visitExpr(ast::ExprId id, int parent_prec = -1);
    void emitLit(const ast::LitValue &node);
    void emitIdent(const ast::IdentNode &node);
    void emitBinary(const ast::BinaryNode &node);
    void emitUnary(const ast::UnaryNode &node);
    void emitCall(const ast::CallNode &node);
    void emitBlock(const ast::BlockNode &node);
    void emitIf(const ast::IfNode &node);
    void emitWhile(const ast::WhileNode &node);
    void emitField(const ast::FieldNode &node);
    void emitIndex(const ast::IndexNode &node);
    void emitStructLiteral(const ast::StructLiteralNode &node);
    void emitArrayLiteral(const ast::ArrayLiteralNode &node);
    void emitEnumValue(const ast::EnumValueNode &node);
    void emitRange(const ast::RangeNode &node);
    void emitWhen(const ast::WhenNode &node);
    void emitIntrinsic(const ast::IntrinsicNode &node);
    void emitMacroCall(const ast::MacroCallNode &node);
    void emitSequence(const ast::SeqNode &node, int parent_prec);
    void emitWordCall(const ast::WordCallNode &node);

    // Type expressions
    void emitType(ast::TypeExprId id);
    void emitBuiltinType(ast::BuiltinType kind);

    // Precedence helpers
    static int binopPrecedence(ast::BinaryOp op);
    static const char *binopText(ast::BinaryOp op);
    static const char *unaryopText(ast::UnaryOp op);
    static bool isLeftAssoc(ast::BinaryOp op);

    // Output helpers
    void indent();
    void newline();
    void emit(std::string_view text);

    template <typename Container, typename Fn>
    void emitCommaList(const Container &items, Fn &&emit_one) {
        for (size_t i = 0; i < items.size(); i++) {
            if (i > 0)
                emit(", ");
            emit_one(items[i]);
        }
    }

    void emitBraceBlock(auto &&body) {
        emit(" {");
        newline();
        indent_++;
        body();
        indent_--;
        indent();
        emit("}");
        newline();
    }
};

} // namespace zith::formatter
