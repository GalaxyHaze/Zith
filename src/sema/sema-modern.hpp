#pragma once

#include "comptime/generic-instantiate.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/optional.hpp"
#include "memory/span.hpp"
#include "sema/modern-types.hpp"
#include "session/frontend-context.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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
    memory::FlatMap<uint32_t, frontend::DeclId> forInNext;
    memory::FlatMap<uint32_t, uint32_t> forInElementIndex;
    memory::FlatMap<uint32_t, uint32_t> forInEndIndex;
    memory::FlatMap<uint32_t, TypeId> forInUnionType;

    explicit TypedMap(memory::Arena &)
        : exprTypes(), declTypes(), localTypes(), forInNext(), forInElementIndex(), forInEndIndex(),
          forInUnionType() {}
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
    void checkZithDeclarations();
    void checkConstFieldAssignments();
    /// Zith-- requires constant expressions for `const` declarations.
    bool isConstantExpression(frontend::ExprId id) const;
    /// True when any field along a place-expression path is a const struct field.
    bool targetFieldIsConst(frontend::ExprId id) const;
    bool findConstField(std::string_view struct_name, size_t index) const noexcept;
    /// Local ids whose uninitialized `let` was typed by its first assignment.
    std::vector<uint32_t> typeInferredByAssignment_;
    TypeId currentReturnType_ = kInvalidTypeId;

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
    std::unordered_map<uint32_t, uint32_t> stateMachineByDecl_;
    /// Machine id per local (parent-body) scope; local states never take part
    /// in the module-level return-type grouping.
    std::unordered_map<uint32_t, uint32_t> localStateMachineByParent_;
    /// Canonical return type -> machine id for the current module.
    std::unordered_map<uint32_t, uint32_t> stateMachineByReturn_;
    /// Next state machine id for the current module.
    uint32_t nextStateMachineId_ = 1;

    /// Generic parameter bindings per declaration (decl id → param name/type pairs).
    /// Populated while lowering declaration types; consulted by `lowerTypeExpr` so
    /// `T` inside a generic declaration resolves to an opaque GenericParam type.
    struct GenericBinding {
        std::string name;
        TypeId type;
    };
    std::unordered_map<uint32_t, std::vector<GenericBinding>> genericParams_;
    /// Overrides for the generic parameters of the template currently being
    /// instantiated in a type expression. `lowerTypeExpr` checks these before
    /// the declaration bindings so `Pair<i32,f64>` fields see `i32`/`f64`.
    std::vector<GenericBinding> activeTemplateArgs_;
    /// Decl id of the declaration currently being lowered or inferred (0 = none).
    uint32_t currentDeclId_ = 0;

    /// Whether the resolved binding's function accepts a trailing variadic tail.
    [[nodiscard]] static bool bindingIsVariadic(const session::ResolvedName &binding) noexcept;

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
    TypeId inferExpr(frontend::ExprId id);
    TypeId inferLiteral(frontend::ExprId id, std::string_view text);
    TypeId inferName(frontend::ExprId id, std::string_view text);
    TypeId inferUnary(frontend::ExprId id);
    TypeId inferBinary(frontend::ExprId id);
    TypeId inferCall(frontend::ExprId id);
    /// Try to resolve a Field/Arrow callee as a method call.
    /// Returns the result type, or kInvalidTypeId (with a diagnostic)
    /// when the field is not a method.
    TypeId inferMethodCall(const frontend::Expression &call, const frontend::Expression &callee);
    TypeId inferBlock(frontend::ExprId id);
    TypeId inferIf(frontend::ExprId id);
    TypeId inferWhile(frontend::ExprId id);
    TypeId inferFor(frontend::ExprId id);
    /// Duck-typed iterator check for `for (name in iterable) { body }`.
    TypeId inferForIn(frontend::ExprId id);
    TypeId inferReturn(frontend::ExprId id);
    TypeId inferAssign(frontend::ExprId id);
    /// Root name of a place expression (`p.inner.x` -> `p`), or an empty id.
    frontend::ExprId assignmentRoot(frontend::ExprId id) const noexcept;
    /// Reports `WriteThroughView` when the assignment target is rooted at a `view` binding.
    void checkAssignableOwnership(frontend::ExprId target, frontend::TextSpan span);
    /// Reports `UnsupportedSyntax` when an assignment target rooted at a `let`/`const`
    /// binding reaches a field, arrow, or index path. The immutable root propagates to
    /// nested struct/union storage; `view` is reported separately by
    /// `checkAssignableOwnership`, and `lend` remains mutable.
    void checkImmutableRootFieldWrite(frontend::ExprId target, frontend::TextSpan span);
    TypeId inferOptionalProp(frontend::ExprId id);
    TypeId inferIndex(frontend::ExprId id);
    TypeId inferSliceRange(frontend::ExprId id);
    void prepareLValueIndexTypes(frontend::ExprId id);
    /// Decodes an integer literal node, including unary `-N`, for static slice checks.
    bool constantIntegerValue(frontend::ExprId id, std::int64_t &out) const noexcept;
    TypeId inferField(frontend::ExprId id);
    TypeId inferArrow(frontend::ExprId id);
    /// When `operand` is a Name that resolves to an enum declaration, returns the enum type
    /// if `variant` names a known variant and reports NoMember otherwise. Returns nullopt
    /// (without diagnostics) when the operand is not an enum-declaration name, so an
    /// enum-typed *value* still reports the plain field-access error.
    memory::Optional<TypeId> enumVariantType(frontend::ExprId operand, std::string_view variant,
                                             frontend::TextSpan span);
    TypeId inferStructLiteral(frontend::ExprId id);
    /// Infers `Union { member }`, the positional construction syntax for a
    /// raw union's first/selected member storage.
    TypeId inferUnionLiteral(frontend::ExprId id, TypeId union_tid, const UnionType &union_data);
    TypeId inferArrayLiteral(frontend::ExprId id);
    TypeId inferCast(frontend::ExprId id);
    /// True for the two supported pointer casts: `raw opaque as *T` and `*T as raw opaque`.
    [[nodiscard]] bool isOpaquePointerCast(TypeId from, TypeId to) const;
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

    /// Resolved method declaration plus the module that owns it. Method
    /// ownership matters at call lowering: imported methods live in the
    /// declaring module's frontend snapshot, not in the caller's.
    struct ResolvedMethod {
        session::ModuleKey module;
        const frontend::Declaration *decl = nullptr;
    };
    /// All methods named `method_name` on `owner_name`, searching the current
    /// module first (preserving existing overload behavior) and then every
    /// other available module in deterministic snapshot order.
    std::vector<ResolvedMethod> findMethodsForOwner(std::string_view owner_name,
                                                    std::string_view method_name) const;

    /// Candidate set for an overloaded call: one entry per visible declaration.
    struct OverloadCandidate {
        const session::ResolvedName *binding = nullptr;
        TypeId type                          = kInvalidTypeId;
        const FunctionType *fn               = nullptr;
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
                                            const std::vector<OverloadCandidate> &candidates,
                                            size_t implicit_args, bool &reported);
    /// Non-mutating form of `adaptNumericLiteral`, used while probing candidates.
    bool literalAdaptsTo(frontend::ExprId value, TypeId target) const noexcept;

    /// Adapts a numeric literal operand to `target` when possible. This is the only implicit
    /// numeric conversion the language keeps; conversions between variables need `as`.
    bool adaptNumericLiteral(frontend::ExprId value, TypeId target);
    /// `coercesTo` plus literal adaptation, for sites that know the source expression.
    bool coerceValue(frontend::ExprId value, TypeId target, TypeId source);

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
    /// Default expression of struct field `field_index` in the named struct, or an empty id.
    frontend::ExprId findFieldDefault(std::string_view struct_name,
                                      size_t field_index) const noexcept;

    TypeId typeOfDeclInModule(session::ModuleKey module, frontend::DeclId id) const noexcept;

public:
    /// Declaration chosen for an overloaded call, keyed by the callee expression.
    /// HIR lowering consults this so it does not re-resolve to the first candidate.
    struct CallTarget {
        session::ModuleKey module;
        frontend::DeclId decl;
    };
    [[nodiscard]] const CallTarget *resolvedCallTarget(frontend::ExprId callee) const noexcept;

private:
    void setResolvedCallTarget(frontend::ExprId callee, session::ModuleKey module,
                               frontend::DeclId decl);
    /// Generic call resolution results, shared with HIR lowering.
    comptime::GenericInstantiationPass *instantiations = nullptr;
    std::unordered_map<uint32_t, CallTarget> call_targets_;
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
