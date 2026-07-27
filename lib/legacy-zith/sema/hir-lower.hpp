#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "legacy-zith/sema/sema-context.hpp"
#include "sema/typed-ast.hpp"
#include "symbols/import-manager.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"
#include "types/unify.hpp"

namespace zith::sema {

class HirLower {
    SemaContext ctx_;
    types::Unifier unifier_;
    memory::Arena &hir_arena_;
    hir::HirModule hir_;
    const TypedAst &typed_ast_;
    hir::HirFunction *current_fn_                     = nullptr;
    const memory::DynArray<symbols::SymId> *resolved_ = nullptr;
    symbols::ImportManager *import_mgr_               = nullptr;
    memory::DynArray<symbols::SymId> worklist_;
    memory::DynArray<symbols::SymId> fn_syms_;
    ast::AstBuilder *active_builder_ = nullptr;
    memory::DynArray<size_t> allowed_files_;
    memory::DynArray<types::TypeId> var_types_;
    types::TypeId current_fn_ret_type_ = types::kErrorType;
    size_t currentBlock_               = 0;
    memory::FlatMap<std::string_view, size_t> labelMap_;
    uint32_t slot_counter_ = 0;

    ast::AstBuilder &builder() const;
    ast::AstBuilder *builderForSym(symbols::SymId fn_sym);
    const symbols::SymbolTable *symsForSym(symbols::SymId fn_sym);
    size_t hirIndexForSym(symbols::SymId fn_sym) const;
    bool isSymAccessible(symbols::SymId sym_id) const;
    types::TypeId astExprType(ast::ExprId id) const;
    hir::HirExprId addHirExpr(hir::HirExpr expr);
    void ensureStub(symbols::SymId fn_sym);
    void ensureBodyLowered(symbols::SymId fn_sym);
    hir::HirExprId visitExpr(ast::ExprId id);
    hir::HirExprId visitLiteral(ast::ExprId id, const ast::LitValue &n);
    hir::HirExprId visitStructLiteral(ast::ExprId id, const ast::StructLiteralNode &n);
    hir::HirExprId visitArrayLiteral(ast::ExprId id, const ast::ArrayLiteralNode &n);
    hir::HirExprId visitEnumValue(ast::ExprId id, const ast::EnumValueNode &n);
    hir::HirExprId visitIdent(const ast::IdentNode &n, ast::ExprId id);
    hir::HirExprId visitBinary(ast::ExprId id, const ast::BinaryNode &n);
    hir::HirExprId visitUnary(ast::ExprId id, const ast::UnaryNode &n);
    hir::HirExprId visitSequence(ast::ExprId id);
    hir::HirExprId visitCall(ast::ExprId id, const ast::CallNode &n);
    hir::HirExprId visitBlock(const ast::BlockNode &n);
    hir::HirExprId visitIf(const ast::IfNode &n);
    hir::HirExprId visitWhile(const ast::WhileNode &n);
    hir::HirExprId visitWhen(ast::ExprId id, const ast::WhenNode &n);
    hir::HirExprId lowerWhenCondition(ast::ExprId cond, hir::HirSlotId subj_slot,
                                      types::TypeId subject_type);
    void visitStmt(ast::StmtId id);
    size_t newBlock();
    hir::HirExprId emitSlotAlloca(hir::HirSlotId slot, types::TypeId type);
    hir::HirExprId emitSlotStore(hir::HirSlotId slot, hir::HirExprId value);
    hir::HirExprId emitSlotLoad(hir::HirSlotId slot, types::TypeId type);
    hir::HirSlotId allocSlot();
    void setCurrentBlock(size_t idx);
    size_t currentBlock() const;
    void setTerminator(hir::HirExprId term);
    void emitJump(size_t target);
    void visitMarker(const ast::MarkerNode &n);
    void visitGoto(const ast::GotoNode &n);

public:
    HirLower(symbols::SymbolTable &syms, types::TypeIntern &types,
             diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder,
             memory::Arena &hir_arena, const TypedAst &typed_ast,
             const memory::DynArray<symbols::SymId> *resolved = nullptr,
             symbols::ImportManager *import_mgr               = nullptr);

    bool run(const ast::ProgramNode &program);
    hir::HirModule takeHir() {
        return std::move(hir_);
    }
    const hir::HirModule &hir() const {
        return hir_;
    }
};

} // namespace zith::sema
