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
        /// True for the C entry `main` whose source return type was `void` but
        /// whose HIR signature was rewritten to `i32` for the linker.
        bool forced_main_return_i32 = false;
    };

    struct LoopTarget {
        size_t continue_block = 0;
        size_t break_block    = 0;
        std::string label;
        /// Number of active cleanup frames that belong to the enclosing block.
        /// Break/continue emit cleanup from this depth up, excluding function
        /// cleanup that still has to run when the function exits normally.
        size_t cleanup_depth = 0;
    };

    struct CleanupFrame {
        memory::DynArray<hir::HirExprId> exprs;

        explicit CleanupFrame(memory::Arena &arena) : exprs(arena) {}
    };

    struct Narrowing {
        frontend::LocalId local = {};
        types::TypeId type      = types::kInvalidType;
        /// Optional aggregate (`?T`, not `?*T`) from which `type` is the
        /// narrowed payload. `true` only while lowering the branch where the
        /// null check proved the payload is present.
        bool optionalPayload = false;
        /// `true` when narrowing an `opaque` local after `is T`; the slot
        /// still stores `OpaqueTagged`, while `type` is the payload type.
        bool opaquePayload = false;
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
    /// Maps an imported C constant name to its predeclared HIR global name.
    memory::FlatMap<memory::InternedId, memory::InternedId> global_const_by_name_;
    std::vector<FunctionInfo> functions_;
    std::vector<LoopTarget> loop_stack_;
    std::vector<Narrowing> narrowing_stack_;
    std::vector<CleanupFrame> cleanup_stack_;
    /// While lowering `defer { ... }`, statement instructions are collected
    /// here instead of being emitted into the current HIR block immediately.
    memory::DynArray<hir::HirExprId> *defer_body_sink_ = nullptr;
    /// Defer statements seen in the current lexical block. They are lowered
    /// only after every binding in that block has emitted its slot setup, so a
    /// deferred body can safely read a binding declared later in the block.
    std::vector<std::vector<frontend::StmtId>> pending_defers_;
    const session::ModuleArtifact *current_module_                   = nullptr;
    const session::ModuleResolution *current_resolution_             = nullptr;
    const TypedMap *current_types_                                   = nullptr;
    const comptime::GenericInstantiationPass *current_instantiation_ = nullptr;
    const comptime::InstantiationInstance *current_instance_         = nullptr;
    hir::HirFunction *current_fn_                                    = nullptr;
    frontend::ScopeId info_decl_parent_scope_;
    sema::modern::TypeId current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    bool current_fn_is_state_                         = false;
    uint32_t current_state_machine_id_                = 0;
    size_t current_block_                             = 0;
    hir::HirSlotId next_slot_                         = 0;
    std::vector<hir::HirSlotId> local_slots_;
    symbols::SymId next_sym_id_ = 1;

    bool predeclareFunctions();
    bool predeclareGlobalConsts();
    void predeclareInstantiation(session::ModuleKey module_key,
                                 const comptime::InstantiationInstance &instance);
    bool lowerFunctionBodies();
    bool lowerFunctionBody(FunctionInfo &info);
    /// Adds the deferred expressions from `first` to the current block before
    /// its terminator. Frames run top-down; each frame runs reverse order.
    void emitCleanupFrom(size_t first);
    /// Lowers all pending `defer` statements from the current lexical block in
    /// source order.
    bool flushPendingDefers();
    /// Lowers one deferred expression/block into the current cleanup frame.
    bool lowerDeferBody(frontend::StmtId id);
    /// Builds a HIR sequence for a `defer { ... }` body without registering the
    /// writes as ordinary expression statements.
    hir::HirExprId lowerDeferBlock(const frontend::Expression &expr);

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
    /// Module-local deterministic id used for `opaque` tag checks and storage.
    /// The id is stable for the same concrete HIR type inside one module, but is
    /// not shared across modules in this first implementation.
    uint32_t stableConcreteTypeId(types::TypeId type) const;

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
    /// Lowers `defaultId` in the module that declared the callee/parameter.
    /// Default expressions live in the declaring module's frontend snapshot
    /// and typed map, so switching only those context pointers is required.
    hir::HirExprId lowerVisibleDefault(const session::ModuleArtifact &module,
                                       const comptime::InstantiationInstance *instance,
                                       frontend::ExprId default_id);
    /// Lowers a default using the declaring module's typed map, then applies
    /// the coercions recorded by sema for `target_sema` before restoring the
    /// caller's lowering context.
    hir::HirExprId lowerDefaultWithTarget(const session::ModuleArtifact &module,
                                          const comptime::InstantiationInstance *instance,
                                          frontend::ExprId default_id,
                                          sema::modern::TypeId target_sema);
    hir::HirExprId lowerBlock(const frontend::Expression &expr);
    hir::HirExprId lowerIf(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerWhen(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerWhenCondition(frontend::ExprId condition, hir::HirSlotId subject_slot,
                                      types::TypeId subject_type);
    hir::HirExprId lowerWhile(const frontend::Expression &expr);
    hir::HirExprId lowerFor(const frontend::Expression &expr);
    hir::HirExprId lowerForIn(const frontend::Expression &expr);
    hir::HirExprId lowerAssign(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerOptionalBoolean(const frontend::Expression &expr);
    hir::HirExprId lowerOptionalProp(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIndex(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerSliceRange(const frontend::Expression &expr, types::TypeId type);
    /// Applies the representation-level coercions recorded by sema: arrays to
    /// slices, string literals retyped as `[]char`, and `[]char -> *char`.
    /// The slice forms emit `HirMakeSlice`; the pointer form projects the data
    /// pointer from the slice via `HirLayoutIntrinsic::PtrOf`.
    hir::HirExprId lowerCoerceToTarget(types::TypeId target, frontend::ExprId expression,
                                       hir::HirExprId value);
    /// Applies only the missing outer optional layers when `source_sema` is
    /// one or more layers shallower than `target_sema`.
    hir::HirExprId lowerCoerceToOptionalDepth(types::TypeId target,
                                              sema::modern::TypeId target_sema,
                                              sema::modern::TypeId source_sema,
                                              hir::HirExprId value);
    /// Materializes the auto-collected `[...]T` tail as a temporary array and
    /// returns a slice over the whole array. The caller stores the returned
    /// slice as the final argument; bounds are the statically-known `[0, N]`.
    hir::HirExprId lowerVariadicSliceTail(sema::modern::TypeId slice_sema_type,
                                          const std::vector<frontend::ExprId> &tail_exprs);
    /// Converts a concrete aggregate to a `dyn Trait`/`dyn Interface` fat
    /// pointer, materialising the vtable for the concrete source type if the
    /// pairing has not been emitted before.
    hir::HirExprId lowerCoerceToDyn(sema::modern::TypeId target, frontend::ExprId expression,
                                    hir::HirExprId value, sema::modern::TypeId target_sema);
    /// Converts a concrete value to bare `opaque` after sema recorded the
    /// original concrete source type. Emits `HirMakeOpaque`; returns `value`
    /// unchanged when this coercion was not registered for `expression`.
    hir::HirExprId lowerCoerceToOpaque(sema::modern::TypeId target, frontend::ExprId expression,
                                       hir::HirExprId value);
    hir::HirExprId lowerField(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrow(const frontend::Expression &expr, types::TypeId type);
    /// When `operand` is a Name that resolves to an enum declaration and `variant` is a
    /// known variant, returns its discriminant; nullopt otherwise (no diagnostics).
    memory::Optional<int64_t> enumVariantValue(frontend::ExprId operand, std::string_view variant);
    hir::HirExprId lowerStructLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerPackLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrayLiteral(const frontend::Expression &expr, types::TypeId type);
    frontend::ExprId lowerFieldDefault(std::string_view struct_name,
                                       size_t field_index) const noexcept;
    hir::HirExprId lowerCast(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIsNull(const frontend::Expression &expr);
    hir::HirExprId lowerIsType(const frontend::Expression &expr);
    hir::HirExprId lowerLayoutIntrinsic(const frontend::Expression &expr);
    hir::HirExprId lowerCoerceToOptional(types::TypeId target, hir::HirExprId value);
    /// Builds `None` for an optional aggregate, recursively zeroing nested
    /// optionals so `??T` keeps its inner `{payload, tag}` layout intact.
    hir::HirExprId lowerMakeNone(types::TypeId target);
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
