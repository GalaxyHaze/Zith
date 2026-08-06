#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/arena.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/nra-facts.hpp"
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
                   types::TypeIntern &types, memory::StringInterner &interner,
                   const NraFacts *nra = nullptr);

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
    const NraFacts *nra_;
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

    /// Pre-scans a function body, assigning a block index to every `marker` label.
    void collectMarkers(frontend::ExprId id);
    /// Marker label -> HIR block index for the current function.
    memory::FlatMap<std::string, size_t> marker_blocks_;

    types::TypeId lowerType(sema::modern::TypeId type);
    types::TypeId lowerForeignType(const cinterop::Type &type);
    types::TypeId typeOfExpr(frontend::ExprId id);
    types::TypeId typeOfLocal(frontend::LocalId id);

    const frontend::Declaration *findDecl(const session::ModuleArtifact &module,
                                          frontend::DeclId id) const noexcept;
    const session::ResolvedName *findResolvedExpr(frontend::ExprId id) const noexcept;
    /// Declaration sema selected for an overloaded call at this callee, if any.
    const PerModuleSema::CallTarget *overloadTarget(frontend::ExprId callee) const noexcept;
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
    hir::HirExprId lowerWhen(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerWhenCondition(frontend::ExprId condition, hir::HirSlotId subject_slot,
                                      types::TypeId subject_type);
    hir::HirExprId lowerWhile(const frontend::Expression &expr);
    hir::HirExprId lowerFor(const frontend::Expression &expr);
    hir::HirExprId lowerAssign(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerOptionalProp(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIndex(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerField(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrow(const frontend::Expression &expr, types::TypeId type);
    /// When `operand` is a Name that resolves to an enum declaration and `variant` is a
    /// known variant, returns its discriminant; nullopt otherwise (no diagnostics).
    memory::Optional<int64_t> enumVariantValue(frontend::ExprId operand, std::string_view variant);
    hir::HirExprId lowerStructLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrayLiteral(const frontend::Expression &expr, types::TypeId type);
    frontend::ExprId lowerFieldDefault(std::string_view struct_name,
                                       size_t field_index) const noexcept;
    hir::HirExprId lowerCast(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIsNull(const frontend::Expression &expr);
    hir::HirExprId lowerLayoutIntrinsic(const frontend::Expression &expr);
    hir::HirExprId lowerCoerceToOptional(types::TypeId target, hir::HirExprId value);
    sema::modern::TypeId semaTypeOfExpr(frontend::ExprId id);
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
