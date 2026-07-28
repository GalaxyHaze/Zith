#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "symbols/import-manager.hpp"
#include "symbols/symbol-table.hpp"

namespace zith::symbols {

class Resolver {
    SymbolTable &syms_;
    ImportManager &import_mgr_;
    ast::AstBuilder &builder_;
    diagnostics::DiagnosticEngine &diags_;
    memory::DynArray<SymId> resolved_;
    bool aliases_only_ = false;

public:
    Resolver(SymbolTable &syms, ImportManager &import_mgr, ast::AstBuilder &builder,
             diagnostics::DiagnosticEngine &diags);

    void resolveProgram(ast::ProgramNode &program);
    void resolveImportMembers(ast::ProgramNode &program);

    SymId resolvedSym(ast::ExprId id) const;
    bool hasResolvedSym(ast::ExprId id) const;
    memory::DynArray<SymId> takeResolvedTable();

private:
    void resolveDecl(ast::DeclId id);
    void resolveStmt(ast::StmtId id);
    void resolveExpr(ast::ExprId id);

    SymId lookupQualified(std::string_view name);
    SymId lookupUnqualified(std::string_view name);
    SymId followAliases(SymId id) const;
    bool isNamespaceLike(SymKind kind) const;

    void setResolved(ast::ExprId id, SymId sym);
};

} // namespace zith::symbols
