#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/arena.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/sema-modern.hpp"
#include "session/frontend-context.hpp"
#include "types/type-intern.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace zith::sema::modern {

class HirLowerModern {
public:
    HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                   const session::CompilationSnapshot &snapshot, const SemaPipeline &sema,
                   types::TypeIntern &types, memory::StringInterner &interner);

    bool run();
    hir::HirModule takeHir() {
        return std::move(hir_);
    }

private:
    struct FunctionInfo {
        std::string key;
        const session::ModuleArtifact *module = nullptr;
        const frontend::Declaration *decl     = nullptr;
        const cinterop::Function *foreign     = nullptr;
        symbols::SymId sym_id                 = symbols::kInvalidSym;
        size_t hir_index                      = 0;
    };

    struct LoopTarget {
        size_t continue_block = 0;
        size_t break_block    = 0;
    };

    memory::Arena &arena_;
    diagnostics::DiagnosticEngine &diags_;
    const session::CompilationSnapshot &snapshot_;
    const SemaPipeline &sema_;
    types::TypeIntern &types_;
    memory::StringInterner &interner_;
    hir::HirModule hir_;
    memory::FlatMap<uint32_t, types::TypeId> lowered_types_;
    std::vector<FunctionInfo> functions_;
    std::vector<LoopTarget> loop_stack_;
    const session::ModuleArtifact *current_module_       = nullptr;
    const session::ModuleResolution *current_resolution_ = nullptr;
    const TypedMap *current_types_                       = nullptr;
    hir::HirFunction *current_fn_                        = nullptr;
    size_t current_block_                                = 0;
    hir::HirSlotId next_slot_                            = 0;
    std::vector<hir::HirSlotId> local_slots_;
    symbols::SymId next_sym_id_ = 1;

    bool predeclareFunctions();
    bool lowerFunctionBodies();
    bool lowerFunctionBody(FunctionInfo &info);

    types::TypeId lowerType(sema::modern::TypeId type);
    types::TypeId lowerForeignType(const cinterop::Type &type);
    types::TypeId typeOfExpr(frontend::ExprId id);
    types::TypeId typeOfLocal(frontend::LocalId id);

    const frontend::Declaration *findDecl(const session::ModuleArtifact &module,
                                          frontend::DeclId id) const noexcept;
    const session::ResolvedName *findResolvedExpr(frontend::ExprId id) const noexcept;
    const frontend::Declaration *
    resolvedFunctionDecl(const session::ResolvedName &resolved,
                         const session::ModuleArtifact **module_out = nullptr) const noexcept;
    symbols::SymId resolvedFunctionSym(const session::ResolvedName &resolved) const noexcept;
    hir::HirSlotId localSlot(frontend::LocalId id);

    hir::HirExprId lowerExpr(frontend::ExprId id);
    hir::HirExprId lowerLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerName(const frontend::Expression &expr);
    hir::HirExprId lowerUnary(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerBinary(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerCall(const frontend::Expression &expr);
    hir::HirExprId lowerBlock(const frontend::Expression &expr);
    hir::HirExprId lowerIf(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerWhile(const frontend::Expression &expr);
    hir::HirExprId lowerAssign(const frontend::Expression &expr, types::TypeId type);
    bool lowerStatement(frontend::StmtId id, hir::HirExprId &last_value);

    hir::HirExprId addExpr(hir::HirExpr expr);
    hir::HirExprId emitSlotAlloca(hir::HirSlotId slot, types::TypeId type);
    hir::HirExprId emitSlotStore(hir::HirSlotId slot, hir::HirExprId value);
    hir::HirExprId emitSlotLoad(hir::HirSlotId slot, types::TypeId type);
    size_t newBlock();
    void setCurrentBlock(size_t block);
    void setTerminator(hir::HirExprId term);
    void emitJump(size_t target);
};

} // namespace zith::sema::modern
