#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/nra-facts.hpp"
#include "sema/sema-modern.hpp"
#include "session/frontend-context.hpp"
#include "types/type-intern.hpp"

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
        uint64_t key                          = 0;
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
    /// Maps `(interned module id, decl id)` to the predeclared HIR function index.
    memory::FlatMap<uint64_t, size_t> function_index_by_key_;
    std::vector<FunctionInfo> functions_;
    std::vector<LoopTarget> loop_stack_;
    const session::ModuleArtifact *current_module_       = nullptr;
    const session::ModuleResolution *current_resolution_ = nullptr;
    const TypedMap *current_types_                       = nullptr;
    hir::HirFunction *current_fn_                        = nullptr;
    bool current_fn_is_flow_                             = false;
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
    memory::FlatMap<memory::InternedId, size_t> marker_blocks_;
    /// Marker label -> stackful flag for the current function.
    memory::FlatMap<memory::InternedId, uint8_t> marker_stackful_;
    /// Marker label -> statement id of its declaration in the current function.
    memory::FlatMap<memory::InternedId, uint32_t> marker_decl_stmts_;
    /// Stack of continuation blocks for active `dock { ... }` bodies in the
    /// ordinary function body. Marker bodies keep their own origin and do not
    /// push onto this stack.
    std::vector<size_t> dockContinuations_;
    /// Nesting depth of `dock` blocks around the current jump statement.
    uint32_t dockDepth_ = 0;
    /// True while lowering a hoisted marker clone.
    bool inMarkerBody_ = false;
    /// Return block of the dock that started the marker flow being lowered.
    size_t activeMarkerReturnBlock_ = ~size_t{0};
    /// One lowered clone per `jump marker` call site. Each clone has its own
    /// entry block and returns to its originating dock continuation.
    struct FlowMarkerInvocation {
        uint32_t marker_stmt;
        size_t entry_block;
        size_t return_block;
    };
    std::vector<FlowMarkerInvocation> markerInvocations_;
    /// Jump statement id -> index into `markerInvocations_`.
    memory::FlatMap<uint32_t, size_t> markerInvocationIndex_;
    /// Blocks whose bodies are hoisted markers; emitted after the main body.
    void lowerHoistedMarkers();
    void lowerMarkerInvocation(const FlowMarkerInvocation &invocation);
    /// Returns the entry block for the fixed, per-call-site marker clone.
    size_t markerInvocationEntry(uint32_t jump_stmt, uint32_t marker_stmt, size_t return_block);
    /// Returns the block a flow jump should return to when its marker falls through.
    size_t flowReturnBlock() const noexcept;

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
    void emitFlowJump(size_t target, size_t return_block);
};

} // namespace zith::sema::modern
