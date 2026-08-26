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
#include <unordered_map>
#include <vector>

namespace zith::sema::modern {

class HirLowerModern {
public:
    HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                   const session::CompilationSnapshot &snapshot, SemaPipeline &sema,
                   types::TypeIntern &types, memory::StringInterner &interner,
                   const NraFacts *nra = nullptr);

    bool run();
    hir::HirModule takeHir() {
        return std::move(hir_);
    }

private:
    struct FunctionInfo {
        uint64_t key                                    = 0;
        const session::ModuleArtifact *module           = nullptr;
        const frontend::Declaration *decl               = nullptr;
        const cinterop::Function *foreign               = nullptr;
        const comptime::InstantiationInstance *instance = nullptr;
        symbols::SymId sym_id                           = symbols::kInvalidSym;
        size_t hir_index                                = 0;
    };

    struct LoopTarget {
        size_t continue_block = 0;
        size_t break_block    = 0;
    };

    struct Narrowing {
        frontend::LocalId local = {};
        types::TypeId type      = types::kInvalidType;
        /// Optional aggregate (`?T`, not `?*T`) from which `type` is the
        /// narrowed payload. `true` only while lowering the branch where the
        /// null check proved the payload is present.
        bool optionalPayload = false;
    };

    memory::Arena &arena_;
    diagnostics::DiagnosticEngine &diags_;
    const session::CompilationSnapshot &snapshot_;
    SemaPipeline &sema_;
    types::TypeIntern &types_;
    memory::StringInterner &interner_;
    const NraFacts *nra_;
    hir::HirModule hir_;
    memory::FlatMap<uint32_t, types::TypeId> lowered_types_;
    /// Maps `(interned module id, decl id)` to the predeclared HIR function index.
    memory::FlatMap<uint64_t, size_t> function_index_by_key_;
    /// Maps `(interned module id, decl id)` to the predeclared HIR const global name.
    memory::FlatMap<uint64_t, memory::InternedId> global_const_by_key_;
    std::vector<FunctionInfo> functions_;
    std::vector<LoopTarget> loop_stack_;
    std::vector<Narrowing> narrowing_stack_;
    const session::ModuleArtifact *current_module_                   = nullptr;
    const session::ModuleResolution *current_resolution_             = nullptr;
    const TypedMap *current_types_                                   = nullptr;
    const comptime::GenericInstantiationPass *current_instantiation_ = nullptr;
    const comptime::InstantiationInstance *current_instance_         = nullptr;
    hir::HirFunction *current_fn_                                    = nullptr;
    frontend::ScopeId info_decl_parent_scope_;
    bool current_fn_is_state_          = false;
    uint32_t current_state_machine_id_ = 0;
    size_t current_block_              = 0;
    hir::HirSlotId next_slot_          = 0;
    std::vector<hir::HirSlotId> local_slots_;
    symbols::SymId next_sym_id_ = 1;

    bool predeclareFunctions();
    bool predeclareGlobalConsts();
    void predeclareInstantiation(session::ModuleKey module_key,
                                 const comptime::InstantiationInstance &instance);
    bool lowerFunctionBodies();
    bool lowerFunctionBody(FunctionInfo &info);

    /// Synthetic for-in binding statement currently being skipped by
    /// `lowerStatement`; set while lowering the loop body.
    frontend::StmtId current_for_in_binding_stmt_;
    /// The for-in element local currently being skipped by `lowerStatement`.
    /// Compared by id so the synthetic statement is skipped even when the
    /// frontend statement id is unavailable.
    frontend::LocalId current_for_in_binding_local_;
    /// True while lowering the root, non-public, non-extern `fn main` whose
    /// sema return type is void.  The HIR signature is promoted to i32 so the
    /// C runtime sees a well-defined success code.
    bool current_main_void_ = false;

    uint32_t lowerTypeSize(types::TypeId type) noexcept;
    uint32_t lowerTypeAlign(types::TypeId type) noexcept;
    /// Number of payload bytes a tagged union needs to store its member index.
    static uint32_t tagByteCount(uint32_t member_count) noexcept;
    /// The integer HIR type used for a tagged union's member-index tag.
    static types::TypeId tagType(types::TypeIntern &types, uint32_t member_count) noexcept;
    /// `tagType` for the tagged union type `type`; raw unions return kInvalidType.
    static types::TypeId lowerTagType(types::TypeId type, types::TypeIntern &types,
                                      uint32_t member_count) noexcept;
    /// Index of `member` in the tagged union `type`, or ~0U when not applicable.
    uint32_t taggedMemberIndex(types::TypeId union_type, types::TypeId member) noexcept;
    /// Rebuilds a tagged-union local by storing a payload value after checking its tag.
    hir::HirExprId rebuildTaggedUnion(types::TypeId union_type, hir::HirExprId value,
                                      uint32_t member_index);
    static uint32_t alignUp(uint32_t value, uint32_t align) noexcept;

    types::TypeId lowerType(sema::modern::TypeId type);
    types::TypeId lowerForeignType(const cinterop::Type &type);
    types::TypeId typeOfExpr(frontend::ExprId id);
    types::TypeId typeOfLocal(frontend::LocalId id);
    sema::modern::TypeId semaTypeOfLocal(frontend::LocalId id);

    const frontend::Declaration *findDecl(const session::ModuleArtifact &module,
                                          frontend::DeclId id) const noexcept;
    const session::ResolvedName *findResolvedExpr(frontend::ExprId id) const noexcept;
    /// Declaration sema selected for an overloaded call at this callee, if any.
    const PerModuleSema::CallTarget *overloadTarget(frontend::ExprId callee) const noexcept;
    /// Finds the sema call target for a method callee, walking the current
    /// module and then every other module in snapshot order. Used to replace
    /// the old owner-name fallback for imported methods.
    const frontend::Declaration *
    methodDeclFromTarget(frontend::ExprId callee,
                         const session::ModuleArtifact **module_out = nullptr) const noexcept;
    const frontend::Declaration *
    resolvedFunctionDecl(const session::ResolvedName &resolved,
                         const session::ModuleArtifact **module_out = nullptr) const noexcept;
    symbols::SymId resolvedFunctionSym(const session::ResolvedName &resolved) const noexcept;
    hir::HirSlotId localSlot(frontend::LocalId id);

    hir::HirExprId lowerExpr(frontend::ExprId id);
    hir::HirExprId lowerLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerName(const frontend::Expression &expr);
    /// Address of an addressable frontend expression without loading its value.
    /// Returns kInvalidHirExpr for call results and other non-addressable values;
    /// callers spill those to a slot before taking their address.
    hir::HirExprId lowerLValueAddr(frontend::ExprId id);
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
    hir::HirExprId lowerForIn(const frontend::Expression &expr);
    hir::HirExprId lowerAssign(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerOptionalProp(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIndex(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerSliceRange(const frontend::Expression &expr, types::TypeId type);
    /// Converts an array expression to a slice by borrowing the whole array.
    /// Emits `HirMakeSlice` with statically-known `[0, N]` bounds.
    hir::HirExprId lowerCoerceToSliceIfArray(types::TypeId target, frontend::ExprId expression,
                                             hir::HirExprId value);
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
    hir::HirExprId lowerIsType(const frontend::Expression &expr);
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
