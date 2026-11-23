#pragma once

#include "comptime/generic-instantiate.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/flat-set.hpp"
#include "memory/optional.hpp"
#include "memory/span.hpp"
#include "sema/modern-types.hpp"
#include "session/frontend-context.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::sema::modern {

struct RelatedSpan {
    frontend::TextSpan span{};
    std::string message;
};

/// Sema's decision for a `[...]T` variadic tail. HIR lowering consumes this
/// plan instead of re-deriving whether the last argument is an explicit slice
/// or a sequence of elements that must be auto-collected.
struct VariadicCallPlan {
    /// Index of the variadic slice parameter in the *concrete/substituted*
    /// callee signature, or `fn->params.size()`. For non-variadic calls this
    /// is the count itself, which lowering already treats as an absent tail.
    size_t sliceParam = ~static_cast<size_t>(0);
    /// Resolved slice parameter type. kInvalidTypeId when the call is not a
    /// variadic-slice call.
    TypeId sliceType      = kInvalidTypeId;
    bool isVariadicSlice  = false;
    bool explicitSliceArg = false;
    /// True when the call lowers the tail into a temporary slice. An explicit
    /// slice argument, or a non-variadic call, keeps this false.
    bool autoCollectTail = false;
    /// Source span used to report lowering problems for this call/tail.
    frontend::TextSpan span{};
};

/// One positional argument after expanding marked pack arguments. Pack
/// members retain their source expression and member index for HIR projection.
struct ExpandedCallArgument {
    frontend::ExprId source;
    TypeId type = kInvalidTypeId;
    uint32_t packMember = ~uint32_t{0};
    bool isSliceForwarding = false;
    uint32_t sourceArgument = ~uint32_t{0};

    [[nodiscard]] bool isPackMember() const noexcept {
        return packMember != ~uint32_t{0};
    }
};

struct ExpandedCallArguments {
    std::vector<ExpandedCallArgument> values;
    frontend::ExprId invalidSpread;
};

struct Diagnostic {
    frontend::TextSpan primary_span{};
    std::string message;
    diagnostics::Severity severity = diagnostics::Severity::Error;
    uint32_t code                  = 0;
    memory::DynArray<RelatedSpan> related_spans;

    Diagnostic(memory::Arena &arena, frontend::TextSpan span, std::string msg,
               diagnostics::Severity sev = diagnostics::Severity::Error, uint32_t code_ = 0)
        : primary_span(span), message(std::move(msg)), severity(sev), code(code_),
          related_spans(arena) {}
};

struct TypedMap {
    memory::FlatMap<uint32_t, TypeId> exprTypes;
    memory::FlatMap<uint32_t, TypeId> declTypes;
    memory::FlatMap<uint32_t, TypeId> localTypes;
    /// ForIn expressions over literal integer ranges. HIR lowering uses this
    /// to emit a step-1 counting loop instead of calling `next`.
    memory::FlatSet<uint32_t> forInRangeLiteral;
    /// Iterator data resolved for a `ForIn` expression, keyed by the expression
    /// id. HIR lowering uses these to emit the `next` call and union extraction
    /// without re-resolving the receiver type.
    struct ForInNext {
        session::ModuleKey module;
        frontend::DeclId decl;
    };
    memory::FlatMap<uint32_t, ForInNext> forInNext;
    /// `contains` method resolved for an `x in rhs` binary, keyed by the
    /// binary expression id. When missing, lowering handles the RHS as a
    /// literal range using the open/closed bound flags.
    struct ContainsCall {
        session::ModuleKey module;
        frontend::DeclId decl;
    };
    memory::FlatMap<uint32_t, ContainsCall> containsCall;
    memory::FlatMap<uint32_t, uint32_t> forInElementIndex;
    memory::FlatMap<uint32_t, uint32_t> forInEndIndex;
    memory::FlatMap<uint32_t, TypeId> forInUnionType;
    /// Canonical `next(self): ?T` / `??T` iterator protocol. When this is set,
    /// HIR lowers the loop by branching on the returned optional instead of
    /// reading a legacy tagged-union `End` member.
    memory::FlatMap<uint32_t, TypeId> forInOptionalType;
    /// For `p.Trait.method()`, the base receiver expression id stored for the
    /// intermediate `p.Trait` expression. HIR lowering uses this base instead
    /// of lowering the marker as a field access.
    memory::FlatMap<uint32_t, uint32_t> traitQualifiedReceiverBase;
    /// Concrete type erased by an implicit `T -> opaque` coercion, keyed by
    /// the value expression id. HIR lowering uses this to materialise the
    /// `{ *void, typeId }` payload from the concrete value.
    memory::FlatMap<uint32_t, TypeId> opaqueSourceTypes;
    /// Concrete type erased by an implicit `T -> dyn Trait` coercion whose
    /// lowered payload differs from the dyn value itself.
    memory::FlatMap<uint32_t, TypeId> dynSourceTypes;
    /// Nullable-pointer expressions proven non-null by the enclosing
    /// control-flow narrowing, keyed by `frontend::LocalId`. Deref, arrow,
    /// index, and nullable-pointer coercion consume this proof.
    memory::FlatSet<uint32_t> provenNonNullExprs;
    /// Optional field reads (`h.c`) proven non-null by the enclosing
    /// control-flow narrowing, keyed by a canonical path string (root local
    /// plus field names). This covers receivers that have no `LocalId`.
    memory::FlatSet<std::string> provenNonNullOptionalExprs;
    /// Read expressions that are the payload of a narrowed optional aggregate
    /// (`?S`, `?pack`), keyed by `frontend::ExprId`. Field access records the
    /// base read here; HIR extracts field 0 instead of loading the whole `?T`.
    memory::FlatMap<uint32_t, TypeId> optionalPayloadReads;
    /// Variadic-slice lowering decisions produced by sema for calls, keyed by
    /// the call expression id.
    memory::FlatMap<uint32_t, VariadicCallPlan> variadicCallPlans;
    /// Ordered positional arguments after expanding marked pack operands.
    /// HIR uses these descriptors to project each pack member from one
    /// evaluation of its source expression.
    memory::FlatMap<uint32_t, std::vector<ExpandedCallArgument>> expandedCallArguments;
    /// Ordered positional arguments after expanding marked packs in `jump`.
    memory::FlatMap<uint32_t, std::vector<ExpandedCallArgument>> expandedStmtArguments;
    /// Variadic-slice lowering decisions produced by sema for `jump`
    /// statements, keyed by statement id.
    memory::FlatMap<uint32_t, VariadicCallPlan> variadicStmtPlans;

    explicit TypedMap(memory::Arena &)
        : exprTypes(), declTypes(), localTypes(), forInRangeLiteral(), forInNext(), containsCall(),
          forInElementIndex(), forInEndIndex(), forInUnionType(), forInOptionalType(),
          traitQualifiedReceiverBase(), opaqueSourceTypes(), dynSourceTypes(), provenNonNullExprs(),
          provenNonNullOptionalExprs(), optionalPayloadReads(), variadicCallPlans(),
          expandedCallArguments(), expandedStmtArguments(), variadicStmtPlans() {}
};

class SemaPipeline;

struct PerModuleSema {
    session::ModuleKey module;
    memory::FileId fileId = 0;
    const frontend::FrontendSnapshot &snapshot;
    const session::ModuleResolution &resolution;
    TypeTable &type_table;
    TypedMap &typed_map;
    memory::Arena &arena;
    memory::DynArray<Diagnostic> diagnostics;
    SemaPipeline *owner = nullptr;
    bool debugSema      = false;

    TypeId error_type;
    TypeId invalid_type;
    TypeId void_type;
    TypeId bool_type;
    TypeId char_type;
    TypeId i32_type;
    TypeId i64_type;
    TypeId f32_type;
    TypeId f64_type;
    TypeId null_type;
    TypeId end_type;
    TypeId opaque_type;

    PerModuleSema(session::ModuleKey mod, const frontend::FrontendSnapshot &snap,
                  const session::ModuleResolution &res, TypeTable &tt, TypedMap &tm,
                  memory::Arena &a, memory::FileId file_id = 0, SemaPipeline *owner_ = nullptr);

    bool run();
    bool prepareTypes();
    bool checkExpressions();
    void resetProvisionalInferenceResults();
    bool hasErrors() const noexcept;

    TypeId typeOfExpr(frontend::ExprId id) const noexcept;
    TypeId typeOfDecl(frontend::DeclId id) const noexcept;
    TypeId typeOfLocal(frontend::LocalId id) const noexcept;
    bool getRequestedAlignment(frontend::ExprId id, uint32_t &out);
    void setExprType(frontend::ExprId id, TypeId type);
    void setDeclType(frontend::DeclId id, TypeId type);
    void setLocalType(frontend::LocalId id, TypeId type);

    void report(frontend::TextSpan span, std::string message, uint32_t code = 0);
    void reportNote(frontend::TextSpan span, std::string message);
    void reportWarning(frontend::TextSpan span, std::string message, uint32_t code = 0);
    void semaProbe(const char *fmt, ...) const;

    std::string_view sourceText(frontend::TextSpan span) const noexcept;
    memory::Span toMemorySpan(frontend::TextSpan span) const noexcept;

private:
    friend struct CastOverflowProbe;

    void registerPrimitiveTypes();
    TypeId registerPrimitive(std::string_view name, TypeKind kind, uint8_t bits, bool is_signed);
    void registerNamedTypes();
    void lowerDeclarationTypes();
    void checkComponentDeclarations();
    void checkComponentMethods();
    [[nodiscard]] bool isPureComponentMethodExpr(frontend::ExprId id,
                                                 const frontend::Declaration &method) const;
    void checkComponentFields(const frontend::Declaration &decl, TypeId type,
                              bool allow_generic_params);
    [[nodiscard]] bool isComponentFieldType(TypeId type, bool allow_generic_params,
                                            std::vector<TypeId> &active_types) const;
    void inferExpressionTypes();
    void inferExpressionTypesForDecls();
    void prepareStateMachineGrouping();
    void probeFunctionCallTargets(const frontend::Declaration &decl);
    void inferFunctionReturnType(const frontend::Declaration &decl);
    void recordInferredReturn(TypeId type, frontend::ExprId source,
                              frontend::TextSpan span);
    void checkReturnsAndCalls();
    void checkStructFieldDefaults();
    void checkFunctionDefaults();
    void checkZithDeclarations();
    void checkAlignmentAttributes();
    bool alignmentIntegerValue(frontend::ExprId id, std::int64_t &out, unsigned depth = 0U);
    uint32_t requestedStructAlignment(const frontend::Declaration &decl);
    void checkConstFieldAssignments();
    /// Validates `#[test]` declarations: a test is a free `void` function with
    /// no parameters, and a module cannot declare both `main` and a test.
    void checkTestDeclarations();
    /// Registers generic implementation methods needed by a `dyn Trait` /
    /// `dyn Interface` coercion of `source` into `target`. The methods inherit
    /// the concrete enum/union owner's type arguments and must exist before HIR
    /// lowers the vtable.
    void primeDynImplementations(TypeId target, TypeId source);
    /// Zith-- requires constant expressions for `const` declarations.
    bool isConstantExpression(frontend::ExprId id) const;
    /// True when any field along a place-expression path is a const struct field.
    bool targetFieldIsConst(frontend::ExprId id) const;
    bool findConstField(std::string_view struct_name, size_t index) const noexcept;
    /// Local ids whose uninitialized `let` was typed by its first assignment.
    std::vector<uint32_t> typeInferredByAssignment_;
    TypeId currentReturnType_ = kInvalidTypeId;
    /// Return inference is active while an unannotated function body is
    /// visited before the normal expression sweep.
    bool inferringReturnType_ = false;
    bool suppressDiagnostics_ = false;
    TypeId inferredReturnCandidate_ = kInvalidTypeId;
    bool hasInferredReturnCandidate_ = false;
    bool inferredReturnConflict_ = false;
    frontend::TextSpan inferredReturnConflictSpan_{};
    /// Expected result type for the next expression inferred in a context that
    /// has a known target type (a binding annotation, a declared return type,
    /// or a typed call argument). It feeds contextual generic inference so a
    /// result-only parameter can bind from the surrounding context. It is
    /// consumed (reset) by the first call that reads it, so it never leaks into
    /// a nested callee's own inference.
    TypeId expectedResultType_ = kInvalidTypeId;
    /// Logical move/address state for parameter/local bindings while inferring
    /// a body. A move (address-of, receiver borrow, or ptrOf of a local)
    /// invalidates reads and field writes, while a direct assignment to the
    /// binding revives it. No real SSA versions or phi nodes are created; this
    /// is a per-body dead-state only.
    memory::FlatSet<uint32_t> movedLocals_;
    /// Pointer-valued expressions that are tied to a local binding. A pointer
    /// produced by address-of or ptrOf may not escape this function body
    /// through returns, persistent aggregates, globals, or deferred storage.
    memory::FlatSet<uint32_t> escapingPointerExprs_;
    /// Local bindings initialized directly from an escaping pointer. These are
    /// tracked so returning or storing an alias reports the same E4008.
    memory::FlatSet<uint32_t> escapingPointerLocals_;
    /// Bindings declared without an initializer in the current body. The set
    /// is cleared after the first direct assignment, and reads through `raw`
    /// are the explicit escape hatch.
    memory::FlatSet<uint32_t> uninitializedLocals_;
    /// Locals that are logically initialized before their body statement runs.
    /// The for-in element binding is created without an initializer and typed
    /// by `inferForIn` before the loop body is visited.
    memory::FlatSet<uint32_t> preinitializedLocals_;

    /// True when the callee's signature mentions a generic parameter.
    [[nodiscard]] bool typeContainsGeneric(const FunctionType *fn) const noexcept;

    /// Function kind of the declaration currently being lowered/inferred.
    frontend::FunctionKind currentFunctionKind_ = frontend::FunctionKind::Standard;
    /// True while inferring inside a `state` function body.
    bool inStateBody_ = false;
    /// Machine id assigned to the enclosing `state` function's machine.
    uint32_t currentStateMachineId_ = 0;
    /// Scope of the body currently being inferred, used to resolve local states.
    frontend::ScopeId currentBodyScope_;
    /// Machine ids assigned to state declarations.
    memory::FlatMap<uint32_t, uint32_t> stateMachineByDecl_;
    /// Machine id per local (parent-body) scope; local states never take part
    /// in the module-level return-type grouping.
    memory::FlatMap<uint32_t, uint32_t> localStateMachineByParent_;
    /// Canonical return type -> machine id for the current module.
    memory::FlatMap<uint32_t, uint32_t> stateMachineByReturn_;
    /// Next state machine id for the current module.
    uint32_t nextStateMachineId_ = 1;

    /// Generic parameter bindings per declaration (decl id → param name/type pairs).
    /// Populated while lowering declaration types; consulted by `lowerTypeExpr` so
    /// `T` inside a generic declaration resolves to an opaque GenericParam type.
    struct GenericBinding {
        std::string name;
        TypeId type;
        std::vector<TypeId> bounds;
    };
    memory::FlatMap<uint32_t, std::vector<GenericBinding>> genericParams_;
    /// Canonical implement owner spelling -> interned owner type. Filled from
    /// `ImplementRecord::ownerType` before method signatures are lowered so an
    /// implicit `self` on `?char`/`[]u8` resolves even though those composite
    /// types have no named declaration.
    memory::FlatMap<std::string, TypeId> implementOwnerTypes_;
    /// Overrides for the generic parameters of the template currently being
    /// instantiated in a type expression. `lowerTypeExpr` checks these before
    /// the declaration bindings so `Pair<i32,f64>` fields see `i32`/`f64`.
    std::vector<GenericBinding> activeTemplateArgs_;
    /// Component field checks can be reached from repeated type-lowering
    /// probes that intern structurally identical generic arguments separately.
    memory::FlatSet<std::string> reportedComponentFieldErrors_;
    /// Decl id of the declaration currently being lowered or inferred (0 = none).
    uint32_t currentDeclId_ = 0;
    /// Labels of loops currently being inferred. A non-empty label must be
    /// unique among active loops so it can name a `break`/`continue` target.
    std::vector<std::string> active_loop_labels_;
    /// Type of the current value inside an active `|>` / `also` stage.
    /// Invalid while no stage is being inferred, so `it` outside a chain is
    /// rejected.
    TypeId pipeCurrentType_ = kInvalidTypeId;
    bool pipeObserving_     = false;
    std::vector<uint32_t> *pipeBorrowedUses_ = nullptr;

    /// Whether the resolved binding's function accepts a trailing variadic tail.
    [[nodiscard]] static bool bindingIsVariadic(const session::ResolvedName &binding) noexcept;
    /// True when the binding declares a homogeneous `[...]T` tail parameter.
    [[nodiscard]] static bool bindingIsVariadicSlice(const session::ResolvedName &binding) noexcept;

    /// Returns the function's variadic-slice parameter index when the resolved
    /// binding carries `isVariadicSlice`, otherwise `fn->params.size()`.
    [[nodiscard]] size_t variadicSliceParam(const session::ResolvedName *binding,
                                            const FunctionType *fn) const;

    /// Validates and retypes the `[...]T` tail around the inferred element type.
    /// Returns the slice parameter type when all tail arguments fit. `span` is
    /// used for diagnostics; `args` must hold the arguments to validate.
    [[nodiscard]] TypeId checkVariadicTail(frontend::TextSpan span,
                                           const std::vector<frontend::ExprId> &args,
                                           const FunctionType *fn, size_t slice_index,
                                           bool allow_literals);

    /// Computes the variadic-slice decision for one call-like expression or
    /// state transition. `args` is the full call operand vector for a
    /// function/method/dock call, or `stmt.arguments` for a jump.
    /// `fixed_explicit_args` is the number of fixed user arguments before the
    /// [...]T tail and `signature` must already be the
    /// concrete/substituted function signature.
    [[nodiscard]] VariadicCallPlan
    makeVariadicCallPlan(frontend::TextSpan span, const std::vector<frontend::ExprId> &args,
                         const FunctionType *signature, bool variadic_slice,
                         size_t fixed_explicit_args, const std::vector<bool> &spread_args,
                         bool has_callee_prefix = true);
    /// Spread is only valid on the final argument that occupies a variadic
    /// slice tail. `fixed_explicit_args` excludes implicit method receivers.
    [[nodiscard]] bool spreadMarkersAreValid(const std::vector<bool> &spread_args,
                                             size_t written_args,
                                             size_t fixed_explicit_args) const noexcept;
    /// Expands marked pack operands to their ordered positional members.
    /// Marked slices/arrays remain one argument so the variadic-slice contract
    /// can validate them separately.
    [[nodiscard]] ExpandedCallArguments
    expandCallArguments(const std::vector<frontend::ExprId> &args,
                        const std::vector<bool> &spread_args,
                        bool has_callee_prefix = true);
    /// Validates an expanded variadic-slice tail, including pack members.
    [[nodiscard]] TypeId
    checkExpandedVariadicTail(frontend::TextSpan span,
                              const std::vector<ExpandedCallArgument> &args,
                              TypeId slice_type, size_t first_tail_index,
                              bool allow_literals);

    /// Validates the `[...]T` tail arguments after the first explicit index
    /// in `args`. The element type comes from `slice_type`, so method paths
    /// can pass a slice parameter that includes an implicit receiver.
    [[nodiscard]] TypeId checkVariadicTailArgs(frontend::TextSpan span,
                                               const std::vector<frontend::ExprId> &args,
                                               TypeId slice_type, size_t first_tail_index,
                                               bool allow_literals);

    friend class SemaPipeline;
    friend class HirLowerModern;

    void checkReturnStatement(const frontend::Statement &stmt);
    void inferDockCall(frontend::ExprId id);
    void inferJump(const frontend::Statement &stmt);
    uint32_t stateMachineIdFor(const frontend::Declaration &decl);
    /// Const lookup used after sema has assigned machine ids, including from HIR
    /// lowering and codegen paths that only hold a const PerModuleSema.
    [[nodiscard]] uint32_t stateMachineIdOf(const frontend::Declaration &decl) const noexcept;
    TypeId lowerTypeExpr(frontend::TypeExprId id);
    /// Lowers `type` ignoring its own memory qualifier; `lowerTypeExpr` wraps the result.
    TypeId lowerBareTypeExpr(const frontend::TypeExpression &type);
    /// For `self: lend Owner` / `self: view Owner`, returns the ABI receiver
    /// type `*Owner` (with the written ownership preserved on the pointee).
    /// Other explicit self parameter types pass through unchanged.
    TypeId methodSelfParamType(const frontend::Parameter &param);
    /// ABI type for a borrow parameter (`lend T` / `view T`): a pointer whose
    /// pointee keeps the written ownership qualifier. Value parameters pass
    /// through unchanged.
    TypeId borrowParamType(const frontend::Parameter &param);
    /// True when an expression resolves to a method's receiver parameter. Used
    /// to auto-deref `self.field` without enabling `.field` for arbitrary pointers.
    [[nodiscard]] bool isSelfReceiver(frontend::ExprId id) const noexcept;
    /// True when an expression resolves to a function parameter whose ABI type
    /// is a borrow pointer (`*lend T` / `*view T`). `p.field` auto-derefs such
    /// parameters just like `self.field`.
    [[nodiscard]] bool isBorrowParameter(frontend::ExprId id) const noexcept;
    /// True when `type` is the lowered ABI type of a `lend`/`view` parameter
    /// (a pointer whose pointee keeps a Lend/View qualifier).
    [[nodiscard]] bool isBorrowParamType(TypeId type) const noexcept;
    [[nodiscard]] bool isViewParamType(TypeId type) const noexcept;
    void checkPipeObservationArgument(frontend::ExprId argument, TypeId param_type,
                                      frontend::TextSpan call_span);
    void markPipeCurrentBorrowed(frontend::ExprId expression);
    [[nodiscard]] bool containsPipeCurrent(frontend::ExprId expression) const noexcept;
    /// Builds/returns the named concrete type for a template application. Shared
    /// by type expressions and generic struct literals. The generic arguments
    /// are written in the caller module and are lowered against this snapshot;
    /// the template body is lowered in its declaring module.
    TypeId instantiateTypeExpr(frontend::TextSpan span, std::string_view name,
                               const std::vector<frontend::TypeExprId> &arguments);
    /// Builds/returns the named concrete type (struct/enum/union/alias) when the
    /// arguments are already lowered, including arguments inferred from a struct
    /// literal. The template body is lowered in the module that declares it.
    TypeId instantiateTemplateFromArgs(frontend::TextSpan span,
                                       const frontend::Declaration &template_decl,
                                       const std::vector<TypeId> &args);
    /// Locates a generic type template named `name` that is visible from this
    /// module: a local declaration first, then the imported module hinted by
    /// `importedTemplateModule_` or a `from`-import binding. `out_module` receives
    /// the declaring module. Returns nullptr when no template is visible.
    const frontend::Declaration *findGenericTypeTemplate(std::string_view name,
                                                         session::ModuleKey *out_module) const;
    /// Deduces missing struct literal generic arguments from provided fields,
    /// materializes the concrete struct, then validates values and defaults.
    TypeId resolveGenericStructLiteral(frontend::TextSpan span, const frontend::Expression &expr,
                                       const frontend::Declaration &template_decl, bool named,
                                       std::vector<TypeId> explicit_args);
    TypeId lowerForeignType(const cinterop::Type &type);
    TypeId lowerForeignConstantType(const cinterop::Constant &constant);
    TypeId inferExpr(frontend::ExprId id);
    /// Infers `id` under a known expected result type. The expectation feeds
    /// contextual generic inference (a result-only parameter such as
    /// `fn Ok<T,E>(): Result<T,E>` binds from the annotation) and is consumed
    /// by the first call it reaches. A missing expectation is a plain infer.
    TypeId inferExprExpected(frontend::ExprId id, TypeId expected);
    /// Infers an expression used directly as a control-flow condition. The
    /// expression must be `bool` or an optional `?T`; an optional is tested
    /// implicitly as `x != null` without narrowing its payload.
    TypeId inferCondition(frontend::ExprId id, std::string_view message, frontend::TextSpan span);
    TypeId inferLiteral(frontend::ExprId id, std::string_view text);
    /// Decodes the escape set of a string/char literal body and reports
    /// `E0003` for an unknown or malformed escape. Runs during sema so that
    /// `check` rejects the same literals `build`/`run` would reject, instead
    /// of only failing later in HIR lowering. Returns true when the body is
    /// valid.
    bool checkLiteralEscapes(frontend::ExprId id, std::string_view text);
    TypeId inferName(frontend::ExprId id, std::string_view text);
    TypeId inferUnary(frontend::ExprId id);
    TypeId inferBinary(frontend::ExprId id);
    /// Infers a `|>` / `also` stage. The source is evaluated once into the
    /// active current value; the stage references it explicitly with `it`.
    TypeId inferPipe(frontend::ExprId id);
    /// Infers `it` in the enclosing `|>` / `also` stage.
    TypeId inferPipeCurrent(frontend::ExprId id);
    /// Resolves `x in rhs` through the `Contains` duck-typed protocol
    /// `contains(self, value): bool`. A literal integer/float range is valid
    /// without a user-defined method; the HIR lowers it to bound comparisons.
    TypeId inferContains(frontend::ExprId binary_id, frontend::ExprId value, frontend::ExprId rhs,
                         frontend::TextSpan span);
    TypeId inferCall(frontend::ExprId id, TypeId expected_result = kInvalidTypeId);
    /// Resolves a generic parameter name bound in the current declaration.
    /// Used to identify `T.method(...)` static trait calls over a generic
    /// parameter during method-call inferencing.
    [[nodiscard]] TypeId genericParamTypeByName(std::string_view name) const;
    /// Renders a type argument with the source-level generic parameter name
    /// when the argument is still an unsubstituted `K`/`V` parameter. Concrete
    /// arguments use the ordinary type spelling.
    [[nodiscard]] std::string typeArgumentName(TypeId arg) const;
    /// Try to resolve a Field/Arrow callee as a method call.
    /// Returns the result type, or kInvalidTypeId (with a diagnostic)
    /// when the field is not a method.
    TypeId inferMethodCall(const frontend::Expression &call, const frontend::Expression &callee,
                           TypeId expected_result = kInvalidTypeId);
    TypeId inferBlock(frontend::ExprId id);
    [[nodiscard]] bool calleeIsDiscardable(frontend::ExprId call_id) const noexcept;
    /// Validates a `defer expr;` / `defer { ... }` statement: infers the body
    /// normally and rejects statements that would transfer control away from
    /// the deferred block.
    void checkDeferStatement(const frontend::Statement &stmt);
    void checkExpressionStatement(const frontend::Statement &stmt, bool produces_block_value);
    /// Validates that a deferred body only reads same-block bindings that are
    /// initialized before every exit path from that block.
    void checkDeferCaptures(const frontend::Statement &stmt, const frontend::Expression &block);
    /// True when `id` reaches a block containing `return`, `break`, `continue`,
    /// or `jump`, including nested control-flow bodies.
    [[nodiscard]] bool deferBodyHasControlFlow(frontend::ExprId id) const noexcept;
    [[nodiscard]] bool deferStatementHasControlFlow(const frontend::Statement &stmt) const noexcept;
    /// Path-based termination analysis used by `checkReturnsAndCalls`. A
    /// non-void function whose body has void type may only fall through when
    /// every reachable path is guaranteed to leave the function: `return`,
    /// state `jump`, both arms of an `if`/`else`, a `when` with a default, or
    /// an unbounded loop whose body cannot break out. A trailing value is not
    /// treated as termination here; it is accepted separately through the
    /// final-value coercion check when the body itself has a non-void type.
    [[nodiscard]] bool exprAlwaysTerminates(frontend::ExprId id) const noexcept;
    [[nodiscard]] bool blockAlwaysTerminates(frontend::ExprId id) const noexcept;
    [[nodiscard]] bool statementAlwaysTerminates(const frontend::Statement &stmt) const noexcept;
    /// True when the loop condition is the literal `true`, so the loop is an
    /// unbounded `for` until a transfer appears in the body.
    [[nodiscard]] bool conditionIsAlwaysLiteralTrue(frontend::ExprId id) const noexcept;
    /// True when a block or expression contains a `break`. Used to reject
    /// infinite loops that can exit and therefore fall through the function
    /// body; nested loops are treated conservatively.
    [[nodiscard]] bool statementContainsBreak(const frontend::Statement &stmt) const noexcept;
    [[nodiscard]] bool exprContainsBreak(frontend::ExprId id) const noexcept;
    TypeId inferIf(frontend::ExprId id);
    TypeId inferWhile(frontend::ExprId id);
    TypeId inferFor(frontend::ExprId id);
    /// Duck-typed iterator check for `for (name in iterable) { body }`.
    TypeId inferForIn(frontend::ExprId id);
    TypeId inferReturn(frontend::ExprId id);
    /// Validates a `break`/`continue` statement against active loop labels
    /// and reports escapes from code that is not inside a loop.
    void checkLoopControl(const frontend::Statement &stmt, bool is_break);
    TypeId inferAssign(frontend::ExprId id);
    /// Root name of a place expression (`p.inner.x` -> `p`), or an empty id.
    frontend::ExprId assignmentRoot(frontend::ExprId id) const noexcept;
    /// Reports `E4001 UseAfterMove` when the root of `target` is an already
    /// invalidated local. Assigning to the dead local itself revives it.
    void checkMovedRoot(const frontend::Expression &target);
    /// True when `id` is, or flows from, a pointer tied to local storage.
    [[nodiscard]] bool pointerAliasEscapesScope(frontend::ExprId id) const;
    /// True when a binding type directly stores a pointer, including optional
    /// pointers such as `?*T`. Optional pointers are nullable aliases and do
    /// not themselves mean the escape is meant to become persistent storage.
    [[nodiscard]] bool isPointerStorageType(TypeId id) const;
    /// True when `id` is nested under a unary `raw` expression. Raw reads are
    /// the explicit unchecked read that may bypass logical move dead-state.
    [[nodiscard]] bool containedInRawRead(frontend::ExprId id) const;
    /// Root name of a postfix chain (`raw box.origin.x` returns `box`).
    [[nodiscard]] frontend::ExprId rawRootName(const frontend::Expression &expr) const noexcept;
    /// Reports `WriteThroughView` when the assignment target is rooted at a `view` binding.
    void checkAssignableOwnership(frontend::ExprId target, frontend::TextSpan span);
    /// Reports `UnsupportedSyntax` when an assignment target rooted at a `let`/`const`
    /// binding reaches a field, arrow, or index path. The immutable root propagates to
    /// nested struct/union storage; `view` is reported separately by
    /// `checkAssignableOwnership`, and `lend` remains mutable.
    void checkImmutableRootFieldWrite(frontend::ExprId target, frontend::TextSpan span);
    /// Marks the logical base of a place expression as moved after a receiver
    /// call whose `self` is implicit or `var self`. `view`/`lend` receivers do
    /// not invalidate the caller binding in this phase.
    void invalidateReceiverRoot(frontend::ExprId base);
    TypeId inferOptionalProp(frontend::ExprId id);
    TypeId inferIndex(frontend::ExprId id);
    TypeId inferSliceRange(frontend::ExprId id);
    void prepareLValueIndexTypes(frontend::ExprId id);
    /// Decodes an integer literal node, including unary `-N`, for static slice checks.
    bool constantIntegerValue(frontend::ExprId id, std::int64_t &out) const noexcept;
    TypeId inferField(frontend::ExprId id);
    TypeId inferArrow(frontend::ExprId id);
    /// Returns true when `field_index` of the struct type is visible from the
    /// current module. `Private` is allowed only at the struct's declaring
    /// file; `pub` is always allowed; `mod`/`mod(N)` follows the same depth
    /// rule used for module declarations.
    [[nodiscard]] bool fieldVisible(const StructType &st, size_t field_index) const noexcept;
    /// When `operand` is a Name that resolves to an enum declaration, returns the enum type
    /// if `variant` names a known variant and reports NoMember otherwise. Returns nullopt
    /// (without diagnostics) when the operand is not an enum-declaration name, so an
    /// enum-typed *value* still reports the plain field-access error.
    memory::Optional<TypeId> enumVariantType(frontend::ExprId operand, std::string_view variant,
                                             frontend::TextSpan span);
    TypeId inferStructLiteral(frontend::ExprId id);
    /// `expected` is the annotated pack type of the surrounding binding or
    /// return when one is available, so a bare `null` member can take its
    /// declared member type instead of the `null`/`never` type.
    TypeId inferPackLiteral(frontend::ExprId id, TypeId expected = kInvalidTypeId);
    /// Infers `Union { member }`, the positional construction syntax for a
    /// raw union's first/selected member storage.
    TypeId inferUnionLiteral(frontend::ExprId id, TypeId union_tid, const UnionType &union_data);
    TypeId inferArrayLiteral(frontend::ExprId id);
    TypeId inferCast(frontend::ExprId id);
    /// True for the two supported pointer casts: `raw opaque as *T` and `*T as raw opaque`.
    [[nodiscard]] bool isOpaquePointerCast(TypeId from, TypeId to) const;
    /// Records one annotated call argument root and reports `E4005` when a
    /// second `lend`, or a conflicting `lend`+`view`, uses the same root in one
    /// call. Returns true when the annotation is accepted.
    bool checkOwnershipCoercion(
        frontend::ExprId arg, TypeId param_type,
        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> &seen_roots,
        frontend::TextSpan call_span, bool report_error);
    /// Returns the union member when `from` is a union and `to` names one of
    /// its members; otherwise reports an InvalidCast and returns `error_type`.
    TypeId unionMemberType(frontend::TextSpan span, TypeId union_type, TypeId member);
    /// The pointer type inside `type`, looking through at most one `Optional` (a C pointer
    /// is `?*T`). Invalid when `type` is not a pointer or nullable pointer.
    [[nodiscard]] TypeId pointerBase(TypeId type) const noexcept;
    /// True for `?*T`: an `Optional` whose inner type is a pointer.
    [[nodiscard]] bool isNullablePointer(TypeId type) const noexcept;
    /// True when a nullable-pointer expression was narrowed by the enclosing
    /// control-flow check, so `*T` expects it may be used directly.
    [[nodiscard]] bool exprHasNonNullPointerProof(frontend::ExprId id) const noexcept;
    /// When `id` names a local of an optional aggregate type (`?T` whose
    /// payload is not a pointer) and the enclosing control flow proved the
    /// payload present, returns that payload type. `kInvalidTypeId` otherwise.
    /// `?*T` is excluded: it uses the pointer niche and unwraps in `inferArrow`.
    [[nodiscard]] TypeId provenOptionalAggregatePayload(frontend::ExprId id) const noexcept;
    /// Canonical path key for an optional read (`L12.c.value`), or empty when
    /// the expression is not a local rooted in a field chain. Used to prove a
    /// field read non-null across an `is null` narrowing.
    [[nodiscard]] std::string optionalReadPathKey(frontend::ExprId id) const;
    /// Local proven non-null by a `not (x is null)` condition, if the
    /// condition directly names a local and the local is a nullable pointer.
    [[nodiscard]] frontend::LocalId
    nonNullPointerLocalFromCondition(frontend::ExprId condition) const noexcept;
    /// Reports `NullDerefUnproven` for an unchecked nullable-pointer coercion.
    [[nodiscard]] bool reportUnprovenNullablePointer(frontend::TextSpan span, TypeId target);
    /// True for `*void` and `?*void`, the two spellings a C `void*` can take.
    [[nodiscard]] bool isVoidPointer(TypeId type) const noexcept;
    TypeId inferIsNull(frontend::ExprId id);
    TypeId inferIsType(frontend::ExprId id);
    TypeId inferWhen(frontend::ExprId id);
    TypeId inferRange(frontend::ExprId id);
    void validateWhenPatternIsland(frontend::ExprId island, TypeId subject,
                                   const frontend::Expression *&narrow_cond);
    TypeId inferLayoutIntrinsic(frontend::ExprId id);
    /// Resolves a method call on a `dyn Trait` / `dyn Interface` receiver.
    TypeId inferDynMethodCall(const frontend::Expression &call, const frontend::Expression &callee,
                              TypeId dyn_type);

    /// Resolved method declaration plus the module that owns it. Method
    /// ownership matters at call lowering: imported methods live in the
    /// declaring module's frontend snapshot, not in the caller's.
    struct ResolvedMethod {
        session::ModuleKey module;
        const frontend::Declaration *decl = nullptr;
        /// Non-empty when the method was declared in a trait, including trait
        /// defaults. Impl methods and owner-local methods keep this empty.
        std::string traitName;
        /// True when the declaration is a trait requirement or default, not a
        /// method written directly for the owner.
        bool isTraitMethod = false;
    };
    /// All methods named `method_name` on `owner_name`, searching the current
    /// module first and then only methods reachable through the current
    /// module's import bindings.
    std::vector<ResolvedMethod> findMethodsForOwner(std::string_view owner_name,
                                                    std::string_view method_name) const;
    /// Canonical owner name for a lowered struct type, excluding template
    /// arguments (`Pair<i32>` → `Pair`).
    [[nodiscard]] std::string ownerNameOf(TypeId pointee) const;
    /// True when `name` is one of the generic parameter names visible for the
    /// declaration currently being lowered (including owner-inherited params).
    [[nodiscard]] bool isGenericTypeParamName(std::string_view name,
                                              uint32_t decl_id) const noexcept;
    /// For a concrete union receiver, returns the member types as the
    /// instantiation arguments of the generic owner. Positional unions map
    /// one parameter to one member, so the member storage itself is the
    /// substituted argument vector.
    [[nodiscard]] std::vector<TypeId> unionArgsFor(TypeId type) const noexcept;
    /// Resolves a canonical implementation owner spelling (`i32`, `?char`,
    /// `[]u8`, `Point`) back to the interned type. Returns invalid when the
    /// spelling is not a supported implement target.
    [[nodiscard]] TypeId ownerTypeFromName(std::string_view owner_name) const;
    /// Resolves a concrete struct receiver method call with the given candidate
    /// set. `qualifying_trait` is non-empty for `p.Trait.method()`; it filters
    /// and (for requirements) resolves impl methods without changing
    /// ordinary `p.method()` selection.
    TypeId resolveStructMethodCall(const frontend::Expression &call,
                                   const frontend::Expression &callee,
                                   const std::vector<ResolvedMethod> &methods, TypeId base_type,
                                   TypeId pointee, bool is_pointer,
                                   TypeId expected_result = kInvalidTypeId);

    /// True when `type` satisfies `trait_or_interface`. Traits are nominal and
    /// must be registered; interfaces are structural and compared field-by-field
    /// and method-by-method.
    [[nodiscard]] bool satisfiesConformance(TypeId type, TypeId trait_or_interface) const;
    /// Checks the bounds declared on `generic_decl` against the resolved
    /// concrete `args`. Reports `E3009` once per failing bound.
    void checkGenericConstraints(const frontend::Declaration &generic_decl,
                                 const std::vector<TypeId> &args, frontend::TextSpan span);
    /// Returns the bounds declared for the generic parameter with the given
    /// type in the module's generic metadata.
    [[nodiscard]] std::vector<TypeId> boundsForGenericParam(TypeId generic_type) const;
    /// Validates `implement Owner as Trait` blocks and registers conformance.
    void checkImplementBlocks();
    /// Populates `implementOwnerTypes_` from the parsed implement records.
    void prepareImplementOwners();
    /// Finds the first declaration of `name` with `kind` in the current module
    /// or through the module's visible import bindings. When `out_module` is
    /// non-null it receives the module that declared this name, so callers can
    /// inspect the defining snapshot instead of rescanning every loaded module.
    [[nodiscard]] const frontend::Declaration *findDeclNamed(std::string_view name,
                                                             frontend::DeclKind kind) const;
    [[nodiscard]] const frontend::Declaration *findDeclNamed(std::string_view name,
                                                             frontend::DeclKind kind,
                                                             session::ModuleKey *out_module) const;
    /// True when `type` refers to an `interface` declaration (structural form,
    /// not a nominal trait).
    [[nodiscard]] bool isInterfaceType(TypeId type) const;
    /// Resolves a bare trait spelling through the current module's local
    /// declarations and import bindings instead of the global name registry.
    [[nodiscard]] TypeId resolvedTraitType(std::string_view name) const;
    /// Resolves a bare type spelling through the current module's visible
    /// declarations, `from`/selector imports, and namespace aliases. Type
    /// names are stored globally across modules, so this exists to keep type
    /// lookup stable when multiple modules define a same-named type.
    [[nodiscard]] TypeId resolvedTypeName(std::string_view name) const;
    /// Const introspection for interface field types. Interface fields are
    /// named primitives/structs and are already lowered during declaration
    /// type preparation, so lookup is enough here.
    [[nodiscard]] TypeId lowerTypeExprConst(frontend::TypeExprId id) const;
    /// Replaces `Self` inside a lowered trait signature with the implementing
    /// owner type. Used by trait requirement comparison and default-method
    /// instantiation.
    [[nodiscard]] TypeId substituteSelf(TypeId type, TypeId self,
                                        TypeId trait = kInvalidTypeId) const;

    /// Candidate set for an overloaded call: one entry per visible declaration.
    struct OverloadCandidate {
        const session::ResolvedName *binding = nullptr;
        TypeId type                          = kInvalidTypeId;
        const FunctionType *fn               = nullptr;
        bool variadicSlice                   = false;
        VariadicCallPlan variadicPlan;
        frontend::TextSpan span{};
        session::ModuleKey module;
        frontend::DeclId decl;
    };

    /// Function type of a binding (declaration, import, or foreign function).
    TypeId typeOfResolvedBinding(const session::ResolvedName &binding);
    /// Picks the single candidate every argument fits, reporting `NoMatchingFn`
    /// when none survives and `AmbiguousCall` when more than one does.  Returns
    /// nullptr in both failure cases; `reported` says whether it diagnosed.
    const OverloadCandidate *selectOverload(const frontend::Expression &call,
                                            std::vector<OverloadCandidate> &candidates,
                                            size_t implicit_args, bool &reported);
    /// Non-mutating form of `adaptNumericLiteral`, used while probing candidates.
    bool literalAdaptsTo(frontend::ExprId value, TypeId target) const noexcept;

    /// Adapts a numeric literal operand to `target` when possible. This is the only implicit
    /// numeric conversion the language keeps; conversions between variables need `as`.
    bool adaptNumericLiteral(frontend::ExprId value, TypeId target);
    /// Marks a value that escapes through an implicit `[]char -> *char`
    /// coercion. `raw` is transparent here, so raw slice-to-pointer conversion
    /// still participates in the same escape checking.
    void markSlicePtrCoercionEscaping(frontend::ExprId value);
    /// True for the one representation-level pointer coercion kept in Zith--:
    /// `[]char` viewed as `*char`.
    [[nodiscard]] bool isCharSliceToPointer(TypeId source, TypeId target) const noexcept;
    /// `coercesTo` plus literal adaptation, for sites that know the source expression.
    bool coerceValue(frontend::ExprId value, TypeId target, TypeId source);

    /// True when every missing fixed parameter after `explicit_args` has a
    /// declared default. `receiver_offset` is 1 for `self` methods and 0
    /// otherwise; `slice_index` excludes a variadic `[...]T` tail when set.
    [[nodiscard]] bool
    missingArgsHaveDefaults(const frontend::Declaration &decl, size_t explicit_args,
                            size_t receiver_offset,
                            size_t slice_index = ~static_cast<size_t>(0)) const noexcept;
    /// Default type for `param_index` of `decl`, or null when absent. Uses the
    /// declaring module's sema for imported declarations/methods.
    [[nodiscard]] static TypeId functionDefaultType(const frontend::Declaration &decl,
                                                    size_t param_index,
                                                    const PerModuleSema &decl_sema) noexcept;

    /// Emits the most specific diagnostic for a failed `source -> target` coercion.
    void reportCoercionFailure(frontend::TextSpan span, TypeId target, TypeId source,
                               std::string_view context,
                               uint32_t fallback_code = diagnostics::err::TypeMismatch);

    bool unify(TypeId expected, TypeId actual);
    bool sameType(TypeId a, TypeId b) const noexcept;
    /// True when a value of `source` is acceptable where `target` is expected,
    /// including the implicit `T -> ?T` and `null -> ?T` coercions.
    /// `?*T` is acceptable where `*T` is expected at the type level only when
    /// it was narrowed/proven non-null; the expression-level proof is checked
    /// in `coerceValue`.
    bool allowsUncheckedNullablePointer(TypeId target, TypeId source) const noexcept;
    bool coercesTo(TypeId target, TypeId source) const noexcept;
    TypeId resolve(TypeId t) const noexcept;
    TypeId concreteBase(TypeId t) const noexcept;

    TypeId typeOfLocalByName(frontend::ScopeId scope, std::string_view name);
    /// Name expression id when `name` resolves to a local declared in `scope`.
    const frontend::Expression *nameExpression(frontend::ScopeId scope,
                                               std::string_view name) const noexcept;
    TypeId typeOfResolvedName(frontend::ExprId id);
    const session::ResolvedName *findResolvedExpr(frontend::ExprId id) const noexcept;
    const session::ResolvedName *findResolvedBinding(std::string_view name,
                                                     frontend::ScopeId scope) const noexcept;
    /// Resolves the imported module for a qualified type expression (`std.counter.Counter`).
    [[nodiscard]] session::ModuleKey
    resolveQualifiedPath(const frontend::TypeExpression &type) const noexcept;
    [[nodiscard]] frontend::ScopeId
    currentScopeForType(const frontend::TypeExpression &type) const noexcept;
    /// Default expression of struct field `field_index` in the named struct, or an empty id.
    frontend::ExprId findFieldDefault(std::string_view struct_name,
                                      size_t field_index) const noexcept;

    /// Module whose generic declarations are being reified while lowering an
    /// imported template body. Empty when lowering a local template.
    session::ModuleKey importedTemplateModule_ = {};

    TypeId typeOfDeclInModule(session::ModuleKey module, frontend::DeclId id) const noexcept;
    /// Declaration selected by a resolved name, including imported declarations.
    [[nodiscard]] const frontend::Declaration *
    declarationForResolved(const session::ResolvedName &resolved) const noexcept;

public:
    /// Declaration chosen for an overloaded call, keyed by the callee expression.
    /// HIR lowering consults this so it does not re-resolve to the first candidate.
    struct CallTarget {
        session::ModuleKey module;
        frontend::DeclId decl;
    };
    void setResolvedCallTarget(frontend::ExprId callee, session::ModuleKey module,
                               frontend::DeclId decl);
    [[nodiscard]] const CallTarget *resolvedCallTarget(frontend::ExprId callee) const noexcept;

private:
    /// Generic call resolution results, shared with HIR lowering.
    comptime::GenericInstantiationPass *instantiations = nullptr;
    memory::FlatMap<uint32_t, CallTarget> call_targets_;
};

class SemaPipeline {
public:
    SemaPipeline(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                 const session::CompilationSnapshot &snapshot, bool debugSema = false);

    bool run();
    bool hasErrors() const noexcept;
    PerModuleSema *findModuleSema(session::ModuleKey module) const noexcept;
    const TypedMap *findTypedMap(session::ModuleKey module) const noexcept;
    TypeTable &typeTable() noexcept {
        return type_table_;
    }
    const TypeTable &typeTable() const noexcept {
        return type_table_;
    }
    [[nodiscard]] sema::modern::TypeTable takeTypeTable() {
        return std::move(type_table_);
    }
    void setInstantiations(comptime::GenericInstantiationPass *instantiations) {
        instantiation_pass_ = instantiations;
    }
    [[nodiscard]] comptime::GenericInstantiationPass *instantiations() const noexcept {
        return instantiation_pass_;
    }
    [[nodiscard]] const std::vector<session::ModuleArtifactPtr> &modules() const noexcept {
        return snapshot_.modules();
    }
    /// True when `id` resolves to the `self` receiver in the module's sema.
    [[nodiscard]] bool isSelfReceiver(session::ModuleKey module,
                                      frontend::ExprId id) const noexcept;
    /// True when `id` resolves to a borrow parameter (`*lend T` / `*view T`).
    [[nodiscard]] bool isBorrowParameter(session::ModuleKey module,
                                         frontend::ExprId id) const noexcept;
    [[nodiscard]] bool debugSema() const noexcept {
        return debugSema_;
    }
    TypedMap &typedMap(session::ModuleKey module) noexcept;

private:
    bool inferFunctionReturnTypes();
    memory::Arena &arena_;
    diagnostics::DiagnosticEngine &diags_;
    const session::CompilationSnapshot &snapshot_;
    TypeTable type_table_;
    memory::FlatMap<session::ModuleKey, TypedMap *> typed_maps_;
    memory::DynArray<PerModuleSema *> modules_;
    bool has_errors_                                        = false;
    comptime::GenericInstantiationPass *instantiation_pass_ = nullptr;
    bool debugSema_                                         = false;
};

} // namespace zith::sema::modern
