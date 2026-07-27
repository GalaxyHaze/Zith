#pragma once

#include "legacy-zith/ast/ast-nodes.hpp"

namespace zith::ast {

inline FnDeclNode *asFn(DeclNode &node) {
    return std::get_if<FnDeclNode>(&node);
}

inline const FnDeclNode *asFn(const DeclNode &node) {
    return std::get_if<FnDeclNode>(&node);
}

inline StructDeclNode *asStruct(DeclNode &node) {
    return std::get_if<StructDeclNode>(&node);
}

inline const StructDeclNode *asStruct(const DeclNode &node) {
    return std::get_if<StructDeclNode>(&node);
}

inline EnumDeclNode *asEnum(DeclNode &node) {
    return std::get_if<EnumDeclNode>(&node);
}

inline const EnumDeclNode *asEnum(const DeclNode &node) {
    return std::get_if<EnumDeclNode>(&node);
}

inline UnionDeclNode *asUnion(DeclNode &node) {
    return std::get_if<UnionDeclNode>(&node);
}

inline const UnionDeclNode *asUnion(const DeclNode &node) {
    return std::get_if<UnionDeclNode>(&node);
}

inline ComponentDeclNode *asComponent(DeclNode &node) {
    return std::get_if<ComponentDeclNode>(&node);
}

inline const ComponentDeclNode *asComponent(const DeclNode &node) {
    return std::get_if<ComponentDeclNode>(&node);
}

inline TraitDeclNode *asTrait(DeclNode &node) {
    return std::get_if<TraitDeclNode>(&node);
}

inline const TraitDeclNode *asTrait(const DeclNode &node) {
    return std::get_if<TraitDeclNode>(&node);
}

inline InterfaceDeclNode *asInterface(DeclNode &node) {
    return std::get_if<InterfaceDeclNode>(&node);
}

inline const InterfaceDeclNode *asInterface(const DeclNode &node) {
    return std::get_if<InterfaceDeclNode>(&node);
}

inline ImportNode *asImport(DeclNode &node) {
    return std::get_if<ImportNode>(&node);
}

inline const ImportNode *asImport(const DeclNode &node) {
    return std::get_if<ImportNode>(&node);
}

inline ExprStmtNode *asExprStmt(StmtNode &node) {
    return std::get_if<ExprStmtNode>(&node);
}

inline const ExprStmtNode *asExprStmt(const StmtNode &node) {
    return std::get_if<ExprStmtNode>(&node);
}

inline TypeAliasDeclNode *asTypeAlias(DeclNode &node) {
    return std::get_if<TypeAliasDeclNode>(&node);
}

inline const TypeAliasDeclNode *asTypeAlias(const DeclNode &node) {
    return std::get_if<TypeAliasDeclNode>(&node);
}

inline GlobalDeclNode *asGlobal(DeclNode &node) {
    return std::get_if<GlobalDeclNode>(&node);
}

inline const GlobalDeclNode *asGlobal(const DeclNode &node) {
    return std::get_if<GlobalDeclNode>(&node);
}

inline ExprKind exprKind(const ExprNode &node) {
    return std::visit([](const auto &entry) { return entry.tag; }, node);
}

inline StmtKind stmtKind(const StmtNode &node) {
    return std::visit([](const auto &entry) { return entry.tag; }, node);
}

inline DeclKind declKind(const DeclNode &node) {
    return std::visit([](const auto &entry) { return entry.tag; }, node);
}

} // namespace zith::ast
