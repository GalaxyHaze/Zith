#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/sema-modern.hpp"
#include "sema/sra-facts.hpp"
#include "session/frontend-context.hpp"
#include "types/type-intern.hpp"

#include <string_view>
#include <vector>

namespace zith::cache {
class Store;
}

namespace zith::symbols {
class SymbolTable;
}

namespace zith::sema::modern {

class HirLowerModern {
public:
    HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                   const session::CompilationSnapshot &snapshot, SemaPipeline &sema,
                   const symbols::SymbolTable &symbols, types::TypeIntern &types,
                   memory::StringInterner &interner, const SraFacts *sra = nullptr,
                   cache::Store *cache_store = nullptr, std::string_view test_module = {},
                   std::string_view test_filter = {});

    bool run();
    hir::HirModule takeHir() {
        return std::move(hir_);
    }

    /// One assert/check site inside a `#[test]` function, for the runner's
    /// Debug location table. `failure_id` is 1-based within its test.
    struct TestFailureSite {
        std::string test_name;
        uint32_t failure_id = 0;
        memory::Span span{};
    };
    /// Root-module test functions in declaration order.
    [[nodiscard]] const std::vector<std::string> &testNames() const noexcept {
        return test_report_names_;
    }
    [[nodiscard]] const std::vector<TestFailureSite> &testFailureSites() const noexcept {
        return test_failure_sites_;
    }

private:
    struct FunctionInfo {
        uint64_t key                                    = 0;
        const session::ModuleArtifact *module           = nullptr;
        const frontend::Declaration *decl               = nullptr;
        const cinterop::Function *foreign               = nullptr;
        const comptime::InstantiationInstance *instance = nullptr;
        hir::HirFunctionId hir_function_id              = hir::kInvalidHirFunction;
        size_t hir_index                                = 0;
        /// True for the C entry `main` whose source return type was `void` but
        /// whose HIR signature was rewritten to `i32` for the linker.
        bool forced_main_return_i32 = false;
        /// True for a `#[test]` whose source return type was `void` but whose
        /// HIR signature was rewritten to `i32` so the driver can read the
        /// result slot (0 = pass, N = the Nth assert failed).
        bool forced_test_return_i32 = false;
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
    const symbols::SymbolTable &symbols_;
    types::TypeIntern &types_;
    memory::StringInterner &interner_;
    const SraFacts *sra_;
    hir::HirModule hir_;
    memory::FlatMap<uint32_t, types::TypeId> lowered_types_;
    /// Maps `(interned module id, decl id)` to the predeclared HIR function index.
    memory::FlatMap<uint64_t, size_t> function_index_by_key_;
    /// Maps `(interned module id, decl id)` to the predeclared HIR const global name.
    memory::FlatMap<uint64_t, memory::InternedId> global_const_by_key_;
    /// Maps an imported C constant name to its predeclared HIR global name.
    memory::FlatMap<memory::InternedId, memory::InternedId> global_const_by_name_;
    /// Maps a C runtime symbol name to the HIR function declared for it by
    /// `ensureForeignFunction`, so repeated uses share one declaration.
    memory::FlatMap<memory::InternedId, hir::HirFunctionId> foreign_fn_by_name_;
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
    const session::ModuleArtifact *current_module_ = nullptr;
    cache::Store *cache_store_                     = nullptr;
    std::string test_module_;
    std::string test_filter_;
    const session::ModuleResolution *current_resolution_             = nullptr;
    const TypedMap *current_types_                                   = nullptr;
    const comptime::GenericInstantiationPass *current_instantiation_ = nullptr;
    const comptime::InstantiationInstance *current_instance_         = nullptr;
    hir::HirFunction *current_fn_                                    = nullptr;
    /// Index of `current_fn_` in the module. Kept alongside the pointer so
    /// re-reading the function after a possible `addFn` reallocation is safe.
    size_t current_fn_index_                      = 0;
    const frontend::Declaration *current_fn_decl_ = nullptr;
    frontend::ScopeId info_decl_parent_scope_;
    sema::modern::TypeId current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    bool current_fn_is_state_                         = false;
    uint32_t current_state_machine_id_                = 0;
    size_t current_block_                             = 0;
    hir::HirSlotId next_slot_                         = 0;
    /// Current pipeline slot while lowering a `|>` / `do` stage. Restored
    /// after the stage so a `..` at the wrong nesting level is rejected by
    /// sema instead of leaking into unrelated expressions.
    hir::HirSlotId pipe_current_slot_ = hir::kInvalidHirSlot;
    std::vector<hir::HirSlotId> local_slots_;
    bool predeclareFunctions();
    bool predeclareGlobalConsts();
    void populateHirDeclarations();
    void predeclareInstantiation(session::ModuleKey module_key,
                                 const comptime::InstantiationInstance &instance);
    /// Resolves the concrete HIR symbol for a nested generic call whose
    /// call-site binding still carries the enclosing instance's type
    /// parameters. Substitutes `current_instance_->args`, creates (or reuses)
    /// the concrete instance, predeclares it, and returns its HIR function id.
    /// Returns `hir::kInvalidHirFunction` when the binding is already concrete
    /// or cannot be resolved yet (its arguments still mention a parameter).
    hir::HirFunctionId concretizeCallTarget(const comptime::InstantiationInstance &bound_instance);
    /// HIR function for a concrete instantiation `(module, decl, args)`, or
    /// `hir::kInvalidHirFunction` when no matching instance was predeclared.
    [[nodiscard]] hir::HirFunctionId
    findInstanceFunction(const session::ModuleKey &module, frontend::DeclId decl,
                         const std::vector<sema::modern::TypeId> &args) const noexcept;
    /// Substitutes the enclosing instance's type arguments into a bound
    /// instance's arguments. A no-op outside a generic body or when the bound
    /// instance is already concrete.
    [[nodiscard]] std::vector<sema::modern::TypeId>
    concreteInstanceArgs(const comptime::InstantiationInstance &instance) const;
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
    /// True while lowering a test function body. Asserts return a positive
    /// failure id, while checks emit failure events and continue.
    bool current_fn_is_test_ = false;
    /// Name of the test currently being lowered (empty outside a test).
    std::string current_test_name_;
    /// Next assert/check failure id inside the current test (1-based).
    uint32_t current_test_failure_id_ = 0;
    /// Test functions in declaration order, discovered in `predeclareFunctions`.
    std::vector<std::string> test_names_;
    std::vector<std::string> test_report_names_;
    /// HIR function id of each entry in `test_names_`, in the same order. The
    /// driver calls these to fill the result table.
    std::vector<hir::HirFunctionId> test_fn_ids_;
    /// `(test, failure id) -> source location` for the runner's Debug section.
    std::vector<TestFailureSite> test_failure_sites_;
    /// True once the synthesized `main` driver has been built for this module.
    bool test_driver_built_ = false;

    uint32_t lowerTypeSize(types::TypeId type) const noexcept;
    uint32_t lowerTypeAlign(types::TypeId type) const noexcept;
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
    /// Canonical 128-bit type id used by `at-canonicalType(T)` and opaque
    /// hydration. Delegates to the shared `types::canonicalTypeId` rule derived
    /// from module namespace, ordered fields, and type name.
    types::TypeCanonicalId canonicalTypeId(types::TypeId type) const;
    /// Project-local runtime tag assigned by the persistent cache registry for
    /// `canonical_id`. Falls back to a session-local TypeIntern tag when no
    /// cache store is attached (unit tests and non-cached compilations).
    uint32_t runtimeTagForCanonicalType(const types::TypeCanonicalId &canonical_id) const;

    types::TypeId lowerType(sema::modern::TypeId type);
    sema::modern::TypeId lowerTypeExprConcrete(frontend::TypeExprId id);
    /// First verified C record layouts keyed by name for named sema placeholders.
    memory::FlatMap<memory::InternedId, const cinterop::Type *> foreign_record_types_;
    /// First C enum declarations keyed by name for named sema placeholders.
    memory::FlatMap<memory::InternedId, const cinterop::Type *> foreign_enum_types_;
    /// Registers records and enums reachable from imported functions, globals,
    /// and typedefs before body lowering.
    void registerForeignTypes();
    /// Returns the first verified C record `Type` registered for `name`.
    [[nodiscard]] const cinterop::Type *foreignRecordForName(std::string_view name) const noexcept;
    /// Returns the first C enum `Type` registered for `name`.
    [[nodiscard]] const cinterop::Type *foreignEnumForName(std::string_view name) const noexcept;
    /// Copies fields from a verified C record into the lowered struct type.
    void fillForeignStruct(types::TypeId lowered, const cinterop::Type &record);
    types::TypeId lowerForeignType(const cinterop::Type &type);
    /// Lower a C pointer pointee. Records become named structs without requiring
    /// by-value layout metadata; pointer ABI does not need a record layout.
    types::TypeId lowerForeignPointee(const cinterop::Type &type);
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
    hir::HirFunctionId resolvedFunctionId(const session::ResolvedName &resolved) const noexcept;
    hir::HirSlotId localSlot(frontend::LocalId id);

    hir::HirExprId lowerExpr(frontend::ExprId id);
    hir::HirExprId lowerLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerName(const frontend::Expression &expr);
    /// Lowers a `fn (...) { ... }` literal to a reference to its synthesized
    /// function. The function itself is predeclared and its body lowered with
    /// the ordinary function path.
    hir::HirExprId lowerFunctionLiteral(const frontend::Expression &expr);
    /// Address of an addressable frontend expression without loading its value.
    /// Returns kInvalidHirExpr for call results and other non-addressable values;
    /// callers spill those to a slot before taking their address.
    hir::HirExprId lowerLValueAddr(frontend::ExprId id);
    hir::HirExprId lowerUnary(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerBinary(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerPipe(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerPipeCurrent(const frontend::Expression &expr);
    /// Returns the frontend id of the single `..` placeholder within `stage`,
    /// or zero when none is present.
    [[nodiscard]] frontend::ExprId findPipeCurrent(frontend::ExprId stage) const noexcept;
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
    hir::HirExprId lowerOptionalProp(const frontend::Expression &expr, types::TypeId type);
    /// Lowers a control-flow condition. A `?T` expression is tested as
    /// `x != null`; a bool is lowered normally.
    hir::HirExprId lowerCondition(frontend::ExprId condition);
    hir::HirExprId lowerOptionalCondition(frontend::ExprId id);
    hir::HirExprId lowerMust(const frontend::Expression &expr, types::TypeId type);
    /// Unchecked optional payload extraction for `raw x`.
    hir::HirExprId lowerRawOptional(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerOptionalPayload(const frontend::Expression &expr, types::TypeId type,
                                        bool checked);
    hir::HirExprId lowerIndex(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerSliceRange(const frontend::Expression &expr, types::TypeId type);
    /// Applies the representation-level coercions recorded by sema: arrays to
    /// slices, string literals retyped as `[]char`, and `[]char -> *char`.
    /// The slice forms emit `HirMakeSlice`; the pointer form projects the data
    /// pointer from the slice via `HirLayoutIntrinsic::PtrOf`.
    hir::HirExprId lowerCoerceToTarget(types::TypeId target, frontend::ExprId expression,
                                       hir::HirExprId value);
    /// Retypes a string-literal format message as `[]char` while preserving
    /// the `\#` sentinel for the `print`/`println` runtime scanner.
    hir::HirExprId lowerFormatMessage(frontend::ExprId expression, hir::HirExprId value);
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
    /// Wraps a concrete value into `?T` where `T` is `dyn` or `opaque`, erasing
    /// the payload first so the `Some` stores the fat pointer or tagged opaque
    /// rather than the raw concrete aggregate. Returns `kInvalidHirExpr` when
    /// the optional payload needs no erasure, so the caller falls back to the
    /// plain `lowerCoerceToOptional` wrap.
    hir::HirExprId lowerCoerceToOptionalErased(types::TypeId target_opt_hir,
                                               sema::modern::TypeId target_opt_sema,
                                               frontend::ExprId expression, hir::HirExprId value);
    hir::HirExprId lowerField(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrow(const frontend::Expression &expr, types::TypeId type);
    /// When `operand` is a Name that resolves to an enum declaration and `variant` is a
    /// known variant, returns its discriminant; nullopt otherwise (no diagnostics).
    memory::Optional<int64_t> enumVariantValue(frontend::ExprId operand, std::string_view variant);
    hir::HirExprId lowerStructLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerPackLiteral(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerArrayLiteral(const frontend::Expression &expr, types::TypeId type);
    /// Resolves the `field_index`-th field default of the struct named
    /// `struct_name` in the module that declares the concrete type. The name
    /// may be qualified (`hm.HashMap`) or reified (`HashMap<i64,i64>`); the
    /// search falls back to the current module when no declaring module is
    /// recorded.
    frontend::ExprId
    lowerFieldDefault(std::string_view struct_name, size_t field_index,
                      const session::ModuleArtifact *declaring_module) const noexcept;
    hir::HirExprId lowerCast(const frontend::Expression &expr, types::TypeId type);
    hir::HirExprId lowerIsNull(const frontend::Expression &expr);
    hir::HirExprId lowerIsType(const frontend::Expression &expr);
    hir::HirExprId lowerLayoutIntrinsic(const frontend::Expression &expr);
    /// Lowers the assert, check, and log intrinsics. Asserts return on failure, checks
    /// accumulate failures and continue, and both panic outside a test.
    hir::HirExprId lowerTestAssert(const frontend::Expression &expr);
    hir::HirExprId lowerTestCheck(const frontend::Expression &expr);
    hir::HirExprId lowerTestLog(const frontend::Expression &expr);
    /// Returns the module-local HIR id of a foreign C function `name` with the
    /// given signature, declaring it once on first use. Used by the test
    /// driver and the log intrinsic for `puts`/`putchar`/`write_stdout`.
    hir::HirFunctionId ensureForeignFunction(std::string_view name,
                                             const std::vector<types::TypeId> &params,
                                             types::TypeId result, bool variadic = false);
    /// Synthesizes the test `main` driver as HIR: it calls each `#[test]`
    /// function in declaration order and prints one `zith-test <name> <slot>`
    /// result line per test. Returns true when no driver was needed.
    bool buildTestDriver();
    /// Emits an `i32` literal into the current block.
    hir::HirExprId emitI32Literal(int64_t value);
    /// Appends a `putchar(byte)` call to the current block.
    void emitPutcharLiteral(std::string_view text);
    /// Appends code that prints a non-negative `i32` in decimal via `putchar`,
    /// creating the digit-scaling and digit-emission blocks it needs.
    void emitPrintI32(hir::HirExprId value);
    hir::HirExprId lowerCoerceToOptional(types::TypeId target, hir::HirExprId value);
    /// Builds `None` for an optional aggregate, recursively zeroing nested
    /// optionals so `??T` keeps its inner `{payload, tag}` layout intact.
    hir::HirExprId lowerMakeNone(types::TypeId target);
    /// True when `?T` is represented by the payload itself with a sentinel for
    /// `None` (`?*T` uses a null pointer, `?dyn T` a null data pointer), so a
    /// payload read loads the value directly instead of extracting field 0.
    [[nodiscard]] bool isNicheOptional(types::TypeId optional_hir) const;
    sema::modern::TypeId semaTypeOfExpr(frontend::ExprId id);
    bool lowerStatement(frontend::StmtId id, hir::HirExprId &last_value);
    uint32_t requestedBindingAlignment(frontend::StmtId id);

    hir::HirExprId addExpr(hir::HirExpr expr);
    hir::HirExprId emitSlotAlloca(hir::HirSlotId slot, types::TypeId type,
                                  uint32_t alignmentBytes = 0U);
    hir::HirExprId emitSlotStore(hir::HirSlotId slot, hir::HirExprId value);
    hir::HirExprId emitSlotLoad(hir::HirSlotId slot, types::TypeId type);
    size_t newBlock();
    void setCurrentBlock(size_t block);
    void setTerminator(hir::HirExprId term);
    void emitJump(size_t target);
};

} // namespace zith::sema::modern
