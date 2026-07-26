#pragma once

#include "ast/ast-builder.hpp"
#include "ast/ast-nodes.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "sema/sema-context.hpp"
#include "sema/typed-ast.hpp"
#include "symbols/import-manager.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"
#include "types/unify.hpp"

#include <string>

namespace zith::sema {

class SemaPipeline {
    SemaContext ctx_;
    types::Unifier unifier_;
    memory::Arena &arena_;
    const memory::DynArray<symbols::SymId> *resolved_ = nullptr;
    TypedAst typed_ast_;
    symbols::ImportManager *import_mgr_ = nullptr;
    memory::DynArray<symbols::SymId> worklist_;
    ast::AstBuilder *active_builder_ = nullptr;
    memory::DynArray<size_t> allowed_files_;
    memory::DynArray<types::TypeId> var_types_;
    memory::DynArray<uint8_t> checked_fns_;
    types::TypeId current_fn_ret_type_ = types::kErrorType;
    memory::FlatMap<std::string_view, size_t> labelMap_;
    std::string target_triple_;

    types::TypeId astExprType(ast::ExprId id) const;
    ast::AstBuilder &builder() const;
    bool isSymAccessible(symbols::SymId sym_id) const;
    ast::AstBuilder *builderForSym(symbols::SymId fn_sym);
    const symbols::SymbolTable *symsForSym(symbols::SymId fn_sym);
    symbols::SymId localSymFor(symbols::SymId fn_sym) const;
    const ast::FnDeclNode *fnDeclForSym(symbols::SymId fn_sym,
                                        ast::AstBuilder **source_bld             = nullptr,
                                        const symbols::SymbolTable **source_syms = nullptr);
    types::TypeId lowerFnReturnType(symbols::SymId fn_sym);
    types::TypeId lowerFnParamType(symbols::SymId fn_sym, size_t index);
    void ensureVarTypeCapacity(symbols::SymId sym_id);
    void ensureCheckedCapacity(symbols::SymId sym_id);
    bool isBodyChecked(symbols::SymId sym_id) const;
    void markBodyChecked(symbols::SymId sym_id);
    bool concretizeLiteralExpr(ast::ExprId id, types::TypeId concrete_type, memory::Span span);
    types::TypeId lowerIntrinsicType(ast::ExprId id, memory::Span span);
    bool materializeLayoutIntrinsic(ast::ExprId id, ast::IntrinsicNode &node);
    const ast::StructDeclNode *structDeclForType(types::TypeId type,
                                                 ast::AstBuilder **source_bld = nullptr);
    bool normalizeStructLiteral(ast::ExprId id, ast::StructLiteralNode &node, types::TypeId type);
    ast::ExprId implicitReturnExpr(ast::ExprId body_id) const;
    void ensureBodyChecked(symbols::SymId fn_sym);
    void warnNotImplemented(std::string_view feature, memory::Span span);
    void reportUnsupportedSyntax(std::string_view syntax, memory::Span span);

    types::TypeId visitExpr(ast::ExprId id);
    types::TypeId visitLiteral(ast::ExprId id, const ast::LitValue &n);
    types::TypeId visitIdent(const ast::IdentNode &n, ast::ExprId id);
    types::TypeId visitBinary(ast::ExprId id, const ast::BinaryNode &n);
    types::TypeId visitUnary(ast::ExprId id, const ast::UnaryNode &n);
    types::TypeId visitSequence(ast::ExprId id);
    types::TypeId visitCall(ast::ExprId id, const ast::CallNode &n);
    types::TypeId visitBlock(ast::ExprId id, const ast::BlockNode &n);
    types::TypeId visitIf(ast::ExprId id, const ast::IfNode &n);
    types::TypeId visitWhile(ast::ExprId id, const ast::WhileNode &n);
    types::TypeId visitWhen(ast::ExprId id, const ast::WhenNode &n);
    void visitStmt(ast::StmtId id);
    void visitMarker(const ast::MarkerNode &n);
    void visitGoto(const ast::GotoNode &n);

public:
    SemaPipeline(symbols::SymbolTable &syms, types::TypeIntern &types,
                 diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder,
                 memory::Arena &arena, const memory::DynArray<symbols::SymId> *resolved = nullptr,
                 symbols::ImportManager *import_mgr = nullptr, std::string_view target_triple = {});

    symbols::SymbolTable &syms() noexcept {
        return ctx_.syms();
    }
    bool run(const ast::ProgramNode &program);
    TypedAst takeTypedAst() {
        return std::move(typed_ast_);
    }
    const TypedAst &typedAst() const {
        return typed_ast_;
    }
};

} // namespace zith::sema
