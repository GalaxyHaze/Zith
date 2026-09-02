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
    /// Iterator data resolved for a `ForIn` expression, keyed by the expression
    /// id. HIR lowering uses these to emit the `next` call and union extraction
    /// without re-resolving the receiver type.
    struct ForInNext {
        session::ModuleKey module;
        frontend::DeclId decl;
    };
    memory::FlatMap<uint32_t, ForInNext> forInNext;
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

    explicit TypedMap(memory::Arena &)
        : exprTypes(), declTypes(), localTypes(), forInNext(), forInElementIndex(), forInEndIndex(),
          forInUnionType(), forInOptionalType(), traitQualifiedReceiverBase(), opaqueSourceTypes(),
          dynSourceTypes() {}
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
    bool hasErrors() const noexcept;

    TypeId typeOfExpr(frontend::ExprId id) const noexcept;
    TypeId typeOfDecl(frontend::DeclId id) const noexcept;
    TypeId typeOfLocal(frontend::LocalId id) const noexcept;
    void setExprType(frontend::ExprId id, TypeId type);
    void setDeclType(frontend::DeclId id, TypeId type);
    void setLocalType(frontend::LocalId id, TypeId type);

    void report(frontend::TextSpan span, std::string message, uint32_t code = 0);
    void reportNote(frontend::TextSpan span, std::string message);

    std::string_view sourceText(frontend::TextSpan span) const noexcept;
    memory::Span toMemorySpan(frontend::TextSpan span) const noexcept;

private:
    void registerPrimitiveTypes();
    TypeId registerPrimitive(std::string_view name, TypeKind kind, uint8_t bits, bool is_signed);
    void registerNamedTypes();
    void lowerDeclarationTypes();
    void inferExpressionTypes();
    void inferExpressionTypesForDecls();
    void checkReturnsAndCalls();
    void checkStructFieldDefaults();
    void checkFunctionDefaults();
    void checkZithDeclarations();
    void checkConstFieldAssignments();
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
    /// Decl id of the declaration currently being lowered or inferred (0 = none).
    uint32_t currentDeclId_ = 0;
    /// Labels of loops currently being inferred. A non-empty label must be
    /// unique among active loops so it can name a `break`/`continue` target.
    std::vector<std::string> active_loop_labels_;

    /// Whether the resolved binding's function accepts a trailing variadic tail.
    [[nodiscard]] static bool bindingIsVariadic(const session::ResolvedName &binding) noexcept;
    /// True when the binding declares a homogeneous `[...]T` tail parameter.
    [[nodiscard]] static bool bindingIsVariadicSlice(const session::ResolvedName &binding) noexcept;

    /// Returns the function's variadic-slice parameter index when the resolved
    /// binding carries `isVariadicSlice`, otherwise `fn->params.size()`.
    [[nodiscard]] size_t variadicSliceParam(const session::ResolvedName *binding,
                                            const FunctionType *fn) const;

    /// True when the final argument of a `[...]T` call is an explicit slice
    /// that must be passed as the slice parameter rather than auto-collected
    /// as one element. A concrete final slice still auto-collects when the
    /// slice element is a `dyn Trait`, because the value must be erased before
    /// it can be stored in the tail array.
    [[nodiscard]] bool variadicFinalArgIsExplicitSlice(TypeId slice_type,
                                                       const std::vector<frontend::ExprId> &args,
                                                       size_t fixed_explicit_args) const;

    /// Validates and retypes the `[...]T` tail around the inferred element type.
    /// Returns the slice parameter type when all tail arguments fit. `span` is
    /// used for diagnostics; `args` must hold the arguments to validate.
    [[nodiscard]] TypeId checkVariadicTail(frontend::TextSpan span,
                                           const std::vector<frontend::ExprId> &args,
                                           const FunctionType *fn, size_t slice_index,
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
    /// Builds/returns the named concrete type for a template application. Shared
    /// by type expressions and generic struct literals.
    TypeId instantiateTypeExpr(frontend::TextSpan span, std::string_view name,
                               const std::vector<frontend::TypeExprId> &arguments);
    /// Builds/returns the named concrete struct type when the arguments are already
    /// lowered, including arguments inferred from a struct literal.
    TypeId instantiateStructFromArgs(frontend::TextSpan span,
                                     const frontend::Declaration &template_decl,
                                     const std::vector<TypeId> &args);
    /// Deduces missing struct literal generic arguments from provided fields,
    /// materializes the concrete struct, then validates values and defaults.
    TypeId resolveGenericStructLiteral(frontend::TextSpan span, const frontend::Expression &expr,
                                       const frontend::Declaration &template_decl, bool named,
                                       std::vector<TypeId> explicit_args);
    TypeId lowerForeignType(const cinterop::Type &type);
    TypeId lowerForeignConstantType(const cinterop::Constant &constant);
    TypeId inferExpr(frontend::ExprId id);
    /// Infers an expression used directly as a control-flow condition. The
    /// expression must be `bool` or an optional `?T`; an optional is tested
    /// implicitly as `x != null` without narrowing its payload.
    TypeId inferCondition(frontend::ExprId id, std::string_view message, frontend::TextSpan span);
    TypeId inferLiteral(frontend::ExprId id, std::string_view text);
    TypeId inferName(frontend::ExprId id, std::string_view text);
    TypeId inferUnary(frontend::ExprId id);
    TypeId inferBinary(frontend::ExprId id);
    TypeId inferCall(frontend::ExprId id);
    /// Resolves a generic parameter name bound in the current declaration.
    /// Used to identify `T.method(...)` static trait calls over a generic
    /// parameter during method-call inferencing.
    [[nodiscard]] TypeId genericParamTypeByName(std::string_view name) const;
    /// Try to resolve a Field/Arrow callee as a method call.
    /// Returns the result type, or kInvalidTypeId (with a diagnostic)
    /// when the field is not a method.
    TypeId inferMethodCall(const frontend::Expression &call, const frontend::Expression &callee);
    TypeId inferBlock(frontend::ExprId id);
    /// Validates a `defer expr;` / `defer { ... }` statement: infers the body
    /// normally and rejects statements that would transfer control away from
    /// the deferred block.
    void checkDeferStatement(const frontend::Statement &stmt);
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
    TypeId inferPackLiteral(frontend::ExprId id);
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
    /// True for `*void` and `?*void`, the two spellings a C `void*` can take.
    [[nodiscard]] bool isVoidPointer(TypeId type) const noexcept;
    TypeId inferIsNull(frontend::ExprId id);
    TypeId inferIsType(frontend::ExprId id);
    TypeId inferWhen(frontend::ExprId id);
    TypeId inferRange(frontend::ExprId id);
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
    /// module first (preserving existing overload behavior) and then every
    /// other available module in deterministic snapshot order.
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
                                   TypeId pointee, bool is_pointer);

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
    /// or any module reachable through the compilation session.
    [[nodiscard]] const frontend::Declaration *findDeclNamed(std::string_view name,
                                                             frontend::DeclKind kind) const;
    /// True when `type` refers to an `interface` declaration (structural form,
    /// not a nominal trait).
    [[nodiscard]] bool isInterfaceType(TypeId type) const;
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
    /// TEMPORARY allowance for `?*T` where `*T` is expected; see the definition. Kept as a
    /// named predicate so the next iteration (flow-sensitive narrowing after `is null`) has
    /// exactly one place to remove.
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
                 const session::CompilationSnapshot &snapshot);

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
    TypedMap &typedMap(session::ModuleKey module) noexcept;

private:
    memory::Arena &arena_;
    diagnostics::DiagnosticEngine &diags_;
    const session::CompilationSnapshot &snapshot_;
    TypeTable type_table_;
    memory::FlatMap<session::ModuleKey, TypedMap *> typed_maps_;
    memory::DynArray<PerModuleSema *> modules_;
    bool has_errors_                                        = false;
    comptime::GenericInstantiationPass *instantiation_pass_ = nullptr;
};

} // namespace zith::sema::modern
