#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <cstring>
#include <string>

namespace zith::sema::modern {

const frontend::Declaration *PerModuleSema::findDeclNamed(std::string_view name,
                                                          frontend::DeclKind kind) const {
    return findDeclNamed(name, kind, nullptr);
}

const frontend::Declaration *PerModuleSema::findDeclNamed(std::string_view name,
                                                          frontend::DeclKind kind,
                                                          session::ModuleKey *out_module) const {
    if (out_module != nullptr)
        *out_module = {};
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == kind && decl.name == name) {
            if (out_module != nullptr)
                *out_module = module;
            return &decl;
        }
    }
    // Resolve the name in this module's import table first. `lookupBinding`
    // walks the lexical chain and returns only the binding visible here, so a
    // same-named trait in an unrelated module cannot be picked up based on
    // module iteration order.
    const auto *binding =
        session::lookupBinding(resolution, name, frontend::ScopeId{}, snapshot.scopes());
    if (binding != nullptr && binding->kind == session::ResolutionKind::Import) {
        const auto *target =
            owner != nullptr ? owner->findModuleSema(binding->target.module) : nullptr;
        if (target != nullptr) {
            for (const auto &decl : target->snapshot.declarations()) {
                if (decl.kind == kind && decl.name == name) {
                    if (out_module != nullptr)
                        *out_module = binding->target.module;
                    return &decl;
                }
            }
        }
    }
    // Namespace imports such as `import soon/memory` bind the first path
    // segment (`soon`) rather than each public symbol. Sema sees the target
    // module through that alias, so `Allocator` remains resolvable without
    // falling back to scanning every loaded module.
    if (owner != nullptr) {
        for (const auto &alias : resolution.bindings) {
            if (alias.scope || alias.kind != session::ResolutionKind::ModuleAlias ||
                alias.target.module.empty() || alias.modulePath.empty())
                continue;
            const auto *target = owner->findModuleSema(alias.target.module);
            if (target == nullptr)
                continue;
            for (const auto &decl : target->snapshot.declarations()) {
                if (decl.kind == kind && decl.name == name) {
                    if (out_module != nullptr)
                        *out_module = alias.target.module;
                    return &decl;
                }
            }
        }
    }
    return nullptr;
}

const session::ResolvedName *
PerModuleSema::findResolvedBinding(std::string_view name, frontend::ScopeId scope) const noexcept {
    return session::lookupBinding(resolution, name, scope, snapshot.scopes());
}

session::ModuleKey
PerModuleSema::resolveQualifiedPath(const frontend::TypeExpression &type) const noexcept {
    if (type.segments.size() < 2U)
        return {};
    const auto *binding = session::lookupModuleAliasForPath(
        resolution, type.segments.front(), currentScopeForType(type), snapshot.scopes(),
        type.segments);
    if (binding == nullptr)
        return {};
    return binding->target.module;
}

frontend::ScopeId
PerModuleSema::currentScopeForType(const frontend::TypeExpression &) const noexcept {
    // Type expressions do not carry a lexical scope today; module aliases are
    // top-level bindings, so the module scope is sufficient and unambiguous
    // for qualified module paths.
    return {};
}

bool PerModuleSema::isInterfaceType(TypeId type) const {
    const TypeId resolved = resolve(type);
    const auto *trait_ty  = type_table.trait(resolved);
    return trait_ty != nullptr &&
           findDeclNamed(trait_ty->name, frontend::DeclKind::Interface) != nullptr;
}

TypeId PerModuleSema::resolvedTypeName(std::string_view name) const {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Trait && decl.kind != frontend::DeclKind::Interface &&
            decl.name == name) {
            const TypeId local = typeOfDecl(decl.id);
            if (local)
                return local;
        }
    }
    const auto *binding =
        session::lookupBinding(resolution, name, frontend::ScopeId{}, snapshot.scopes());
    if (binding != nullptr && binding->kind == session::ResolutionKind::Import &&
        !binding->target.module.empty() && owner != nullptr) {
        const auto *target = owner->findModuleSema(binding->target.module);
        if (target != nullptr) {
            for (const auto &decl : target->snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Trait &&
                    decl.kind != frontend::DeclKind::Interface && decl.name == name) {
                    const TypeId imported = target->typeOfDecl(decl.id);
                    if (imported)
                        return imported;
                }
            }
        }
    }
    if (owner != nullptr) {
        for (const auto &alias : resolution.bindings) {
            if (alias.scope || alias.kind != session::ResolutionKind::ModuleAlias ||
                alias.target.module.empty())
                continue;
            const auto *target = owner->findModuleSema(alias.target.module);
            if (target == nullptr)
                continue;
            for (const auto &decl : target->snapshot.declarations()) {
                if (decl.kind == frontend::DeclKind::Trait ||
                    decl.kind == frontend::DeclKind::Interface || decl.name != name)
                    continue;
                const TypeId result = target->typeOfDecl(decl.id);
                if (result)
                    return result;
            }
        }
    }
    return type_table.lookupNamed(name);
}

TypeId PerModuleSema::resolvedTraitType(std::string_view name) const {
    for (const auto &decl : snapshot.declarations()) {
        if ((decl.kind == frontend::DeclKind::Trait ||
             decl.kind == frontend::DeclKind::Interface) &&
            decl.name == name &&
            type_table.kindOf(type_table.lookupNamed(decl.name)) == TypeKind::Trait)
            return type_table.lookupNamed(decl.name);
    }
    const auto *binding =
        session::lookupBinding(resolution, name, frontend::ScopeId{}, snapshot.scopes());
    if (owner != nullptr) {
        const auto findInTarget = [&](session::ModuleKey target_module) -> TypeId {
            const auto *target = owner->findModuleSema(target_module);
            if (target == nullptr)
                return kInvalidTypeId;
            for (const auto &decl : target->snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Trait &&
                    decl.kind != frontend::DeclKind::Interface)
                    continue;
                if (decl.name != name)
                    continue;
                const TypeId result = type_table.lookupNamed(decl.name);
                if (result && type_table.kindOf(result) == TypeKind::Trait)
                    return result;
            }
            return kInvalidTypeId;
        };
        if (binding != nullptr && binding->kind == session::ResolutionKind::Import &&
            !binding->target.module.empty()) {
            const TypeId imported = findInTarget(binding->target.module);
            if (imported)
                return imported;
        }
        if (binding == nullptr || binding->kind == session::ResolutionKind::ModuleAlias) {
            for (const auto &alias : resolution.bindings) {
                if (alias.scope || alias.kind != session::ResolutionKind::ModuleAlias ||
                    alias.target.module.empty() || alias.modulePath.empty())
                    continue;
                const TypeId namespaced = findInTarget(alias.target.module);
                if (namespaced)
                    return namespaced;
            }
        }
    }
    return type_table.lookupNamed(name);
}

TypeId PerModuleSema::lowerTypeExprConst(frontend::TypeExprId id) const {
    if (!id || id.value > snapshot.typeExpressions().size())
        return kInvalidTypeId;
    const auto &type = snapshot.typeExpressions()[id.value - 1U];
    if (type.ownership != frontend::OwnershipKind::Default || type.isMut) {
        frontend::TypeExpression bare = type;
        bare.ownership                = frontend::OwnershipKind::Default;
        bare.isMut                    = false;
        bare.hasMutKeyword            = false;
        // Existing type lowering mutates generic/instantiation state on some
        // paths; the const helper is only used for already-lowered interface
        // field introspection, so reentering it safely is unnecessary.
        (void)bare;
    }
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Interface && decl.name == type.name)
            return type_table.lookupNamed(decl.name);
    }
    return type_table.lookupNamed(type.name);
}

TypeId PerModuleSema::substituteSelf(TypeId type, TypeId self, TypeId trait) const {
    if (!type)
        return type;
    if (const auto *qualified = type_table.qualified(type))
        return type_table.internQualified(substituteSelf(qualified->inner, self, trait),
                                          qualified->ownership, qualified->isMut);
    if (const auto *alias = type_table.alias(type))
        return type_table.internAlias(substituteSelf(alias->target, self, trait));
    if (const auto *nominal = type_table.nominal(type))
        return type_table.internNominal(nominal->name,
                                        substituteSelf(nominal->target, self, trait));
    const TypeId resolved = type_table.stripQualifiers(type);
    // A trait requirement's implicit `self` lowers to `*Trait`. Substituting
    // that pointer as `Self` must yield the concrete owner type itself, so a
    // `*Self` parameter becomes `*Owner`, not `*(*Owner)` or `*Trait`.
    if (const auto *ptr = type_table.pointer(resolved); ptr != nullptr && trait) {
        const TypeId pointee = type_table.stripQualifiers(ptr->pointee);
        if (resolve(pointee) == resolve(trait) ||
            (type_table.kindOf(pointee) == TypeKind::Trait &&
             type_table.kindOf(resolve(trait)) == TypeKind::Trait &&
             type_table.trait(pointee)->name == type_table.trait(resolve(trait))->name)) {
            return type_table.internPointer(self);
        }
        return type_table.internPointer(substituteSelf(ptr->pointee, self, trait));
    }
    if (const auto *opt = type_table.optional(resolved))
        return type_table.internOptional(substituteSelf(opt->inner, self, trait));
    if (const auto *array = type_table.array(resolved))
        return type_table.internArray(substituteSelf(array->element, self, trait), array->size);
    if (const auto *slice = type_table.slice(resolved))
        return type_table.internSlice(substituteSelf(slice->element, self, trait));
    if (const auto *fn = type_table.function(resolved)) {
        auto &params = type_table.makeTypeStorage();
        for (const auto param : fn->params)
            params.push(substituteSelf(param, self, trait));
        return type_table.kindOf(resolved) == TypeKind::State
                   ? type_table.internStateFunction(params, substituteSelf(fn->result, self, trait))
                   : type_table.internFunction(params, substituteSelf(fn->result, self, trait));
    }
    if (trait && resolve(type) == resolve(trait))
        return self;
    return type;
}
TypeId PerModuleSema::lowerTypeExpr(frontend::TypeExprId id) {
    if (!id || id.value > snapshot.typeExpressions().size())
        return kInvalidTypeId;
    const auto &type = snapshot.typeExpressions()[id.value - 1U];
    const TypeId bare{lowerBareTypeExpr(type)};
    // A memory qualifier wraps the type it annotates. `resolve()` sees through the
    // wrapper, so unification, coercion and casts are unchanged, while the
    // qualifier stays recoverable for the mutability checks.
    if (bare && (type.ownership != frontend::OwnershipKind::Default || type.isMut))
        return type_table.internQualified(bare, mapOwnership(type.ownership), type.isMut);
    return TypeId{bare};
}
TypeId PerModuleSema::methodSelfParamType(const frontend::Parameter &param) {
    const TypeId declared = lowerTypeExpr(param.type);
    if (!declared)
        return declared;
    const auto *qualifier = type_table.qualified(type_table.canonical(declared));
    if (qualifier == nullptr || (qualifier->ownership != types::OwnershipKind::Lend &&
                                 qualifier->ownership != types::OwnershipKind::View)) {
        return declared;
    }
    // `self: lend Owner` / `self: view Owner` is a borrow receiver: the
    // method body receives a pointer to Owner, and the qualifier remains on
    // the pointee so ownership/read-only checks still recognize it.
    const TypeId inner        = type_table.stripQualifiers(declared);
    const TypeKind inner_kind = inner ? type_table.kindOf(resolve(inner)) : TypeKind::Error;
    if (!inner || (inner_kind != TypeKind::Struct && inner_kind != TypeKind::Enum &&
                   inner_kind != TypeKind::Union)) {
        return declared;
    }
    return type_table.internPointer(
        type_table.internQualified(inner, qualifier->ownership, qualifier->isMut));
}
TypeId PerModuleSema::borrowParamType(const frontend::Parameter &param) {
    const TypeId declared = lowerTypeExpr(param.type);
    if (!declared)
        return declared;
    const auto *qualifier = type_table.qualified(type_table.canonical(declared));
    if (qualifier == nullptr || (qualifier->ownership != types::OwnershipKind::Lend &&
                                 qualifier->ownership != types::OwnershipKind::View)) {
        return declared;
    }
    const TypeId inner = type_table.stripQualifiers(declared);
    if (!inner)
        return declared;
    return type_table.internPointer(
        type_table.internQualified(inner, qualifier->ownership, qualifier->isMut));
}
bool PerModuleSema::isSelfReceiver(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto *resolved = findResolvedExpr(id);
    if (resolved == nullptr || !resolved->local)
        return false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || decl.ownerName.empty() ||
            decl.parameters.empty() || decl.parameters.front().id != resolved->local)
            continue;
        return decl.parameters.front().name == "self";
    }
    return false;
}
bool PerModuleSema::isBorrowParameter(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto *resolved = findResolvedExpr(id);
    if (resolved == nullptr || !resolved->local)
        return false;
    const TypeId param_type = typeOfLocal(resolved->local);
    return isBorrowParamType(param_type);
}
bool PerModuleSema::isBorrowParamType(TypeId type) const noexcept {
    if (!type)
        return false;
    // The ABI type is a pointer whose pointee is qualified. Iterating a
    // transparent alias or nominal preserves spelling while still recognizing
    // `*lend T` / `*view T` when a borrow type flows through one.
    TypeId current = type;
    for (unsigned guard = 0; guard < 8U; ++guard) {
        current = type_table.canonical(current);
        if (const auto *qualified = type_table.qualified(current); qualified != nullptr) {
            current = qualified->inner;
            continue;
        }
        break;
    }
    const auto *pointer = type_table.pointer(current);
    if (pointer == nullptr)
        return false;
    const auto *qualified = type_table.qualified(type_table.canonical(pointer->pointee));
    return qualified != nullptr && (qualified->ownership == types::OwnershipKind::Lend ||
                                    qualified->ownership == types::OwnershipKind::View);
}
bool PerModuleSema::isViewParamType(TypeId type) const noexcept {
    if (!type)
        return false;
    TypeId current = type;
    for (unsigned guard = 0; guard < 8U; ++guard) {
        current = type_table.canonical(current);
        if (const auto *qualified = type_table.qualified(current); qualified != nullptr) {
            current = qualified->inner;
            continue;
        }
        break;
    }
    const auto *pointer = type_table.pointer(current);
    if (pointer == nullptr)
        return false;
    const auto *qualified = type_table.qualified(type_table.canonical(pointer->pointee));
    return qualified != nullptr && qualified->ownership == types::OwnershipKind::View;
}
void PerModuleSema::checkPipeObservationArgument(frontend::ExprId argument, TypeId param_type,
                                                frontend::TextSpan call_span) {
    if (!containsPipeCurrent(argument))
        return;
    if (isBorrowParamType(param_type))
        markPipeCurrentBorrowed(argument);
    if (pipeObserving_ && !isViewParamType(param_type))
        report(call_span, "an 'also' stage may pass 'it' only to read-only 'view' parameters",
               diagnostics::err::OwnershipCoercionRequired);
}
bool PerModuleSema::checkOwnershipCoercion(
    frontend::ExprId arg, TypeId param_type,
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> &seen_roots,
    frontend::TextSpan call_span, bool report_error) {
    if (!arg || !param_type || arg.value > snapshot.expressions().size())
        return true;
    if (!isBorrowParamType(param_type))
        return true;

    const auto &annotated = snapshot.expressions()[arg.value - 1U];
    const bool has_annotation =
        annotated.kind == frontend::ExprKind::OwnershipCoerce && !annotated.operands.empty();
    const frontend::ExprId inner = has_annotation ? annotated.operands[0] : arg;
    if (!inner || inner.value > snapshot.expressions().size())
        return true;

    TypeId target = param_type;
    for (unsigned guard = 0; guard < 8U; ++guard) {
        target = type_table.canonical(target);
        if (const auto *qualified = type_table.qualified(target); qualified != nullptr) {
            target = qualified->inner;
            continue;
        }
        break;
    }
    const auto *pointer = type_table.pointer(target);
    if (pointer == nullptr)
        return true;
    const auto *pointee_qual = type_table.qualified(type_table.canonical(pointer->pointee));
    if (pointee_qual == nullptr || (pointee_qual->ownership != types::OwnershipKind::Lend &&
                                    pointee_qual->ownership != types::OwnershipKind::View))
        return true;
    const types::OwnershipKind target_ownership = pointee_qual->ownership;

    const auto &inner_expr                = snapshot.expressions()[inner.value - 1U];
    types::OwnershipKind source_ownership = types::OwnershipKind::Default;
    if (has_annotation) {
        source_ownership = mapOwnership(annotated.ownership);
    } else if (const TypeId source_type = typeOfExpr(inner); source_type) {
        if (const auto *initial_qualifier = type_table.qualified(type_table.canonical(source_type));
            initial_qualifier != nullptr &&
            initial_qualifier->ownership != types::OwnershipKind::Default)
            source_ownership = initial_qualifier->ownership;
        TypeId source_cursor = type_table.canonical(source_type);
        for (unsigned guard = 0; guard < 8U; ++guard) {
            if (const auto *source_qualifier = type_table.qualified(source_cursor);
                source_qualifier != nullptr) {
                source_cursor = source_qualifier->inner;
                continue;
            }
            break;
        }
        if (const auto *source_ptr = type_table.pointer(source_cursor); source_ptr != nullptr) {
            const auto *source_qual =
                type_table.qualified(type_table.canonical(source_ptr->pointee));
            if (source_qual != nullptr)
                source_ownership = source_qual->ownership;
        } else {
            const auto *source_qual = type_table.qualified(source_cursor);
            if (source_qual != nullptr)
                source_ownership = source_qual->ownership;
        }
    }

    // Literals, call results and other rvalue/temporary expressions need no
    // call-site annotation even though they cannot be borrowed in place. The
    // semantics is "materialize a temporary where the ABI needs an address".
    const bool place_expression = inner_expr.kind == frontend::ExprKind::Name ||
                                  inner_expr.kind == frontend::ExprKind::Field ||
                                  inner_expr.kind == frontend::ExprKind::Arrow ||
                                  inner_expr.kind == frontend::ExprKind::Index;
    const bool direct_binding = inner_expr.kind == frontend::ExprKind::Name;
    const bool already_borrow = source_ownership == types::OwnershipKind::Lend ||
                                source_ownership == types::OwnershipKind::View;
    const bool annotation_missing = !has_annotation && direct_binding && !already_borrow;

    if (has_annotation && source_ownership != target_ownership) {
        if (report_error) {
            const std::string required =
                target_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            const std::string written =
                source_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            report(annotated.span,
                   "call argument annotation mismatch: parameter expects '" + required +
                       "', call site wrote '" + written + "'",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }
    if (annotation_missing) {
        if (report_error) {
            const std::string required =
                target_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            report(inner_expr.span,
                   "call to '" + std::string(inner_expr.text) + "' needs an explicit '" + required +
                       "' annotation for this borrow parameter",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }

    if (!has_annotation || !place_expression)
        return true;
    // Plain bindings conflict by the local they resolve to, so a call can
    // never lend the same variable twice. Field/index/arrow paths use the full
    // place id: two identical field paths conflict, but distinct fields and
    // distinct receiver roots remain separate borrows in this slice.
    frontend::ExprId conflict_root;
    if (direct_binding) {
        const auto *inner_resolved = findResolvedExpr(inner);
        conflict_root              = inner_resolved != nullptr && inner_resolved->local
                                         ? frontend::ExprId{inner_resolved->local.value}
                                         : inner;
    } else {
        conflict_root = inner;
    }
    if (!conflict_root)
        return true;
    const auto conflict_kind = source_ownership == types::OwnershipKind::Lend
                                   ? types::OwnershipKind::Lend
                                   : types::OwnershipKind::View;
    for (const auto &seen : seen_roots) {
        if (seen.first != conflict_root)
            continue;
        const bool lend_conflict = conflict_kind == types::OwnershipKind::Lend &&
                                   seen.second == types::OwnershipKind::Lend;
        const bool mixed_conflict = (conflict_kind == types::OwnershipKind::Lend &&
                                     seen.second == types::OwnershipKind::View) ||
                                    (conflict_kind == types::OwnershipKind::View &&
                                     seen.second == types::OwnershipKind::Lend);
        if (!lend_conflict && !mixed_conflict)
            continue;
        if (report_error) {
            report(call_span, "the same binding cannot be borrowed more than once in this call",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }
    seen_roots.emplace_back(conflict_root, conflict_kind);
    return true;
}
TypeId PerModuleSema::lowerForeignType(const cinterop::Type &type) {
    switch (type.kind) {
    case cinterop::TypeKind::Void:
        return void_type;
    case cinterop::TypeKind::Bool:
        return bool_type;
    case cinterop::TypeKind::Integer:
        if (type.isChar)
            return char_type;
        return type_table.internInteger({type.bits, type.isSigned});
    case cinterop::TypeKind::Float:
        return type_table.internFloat({type.bits});
    case cinterop::TypeKind::Pointer: {
        // Every C pointer is nullable, so it lowers to `?*T`. The niche representation
        // keeps the LLVM layout identical to a bare pointer, and the pointee stays
        // non-optional so `char **` does not become `?*?*char`.
        const TypeId pointee = type.pointee ? lowerForeignType(*type.pointee) : error_type;
        return type_table.internOptional(type_table.internPointer(pointee));
    }
    case cinterop::TypeKind::Record:
        return type_table.findOrCreateNamed(type.name, TypeKind::Struct);
    case cinterop::TypeKind::Enum:
        return type_table.findOrCreateNamed(type.name, TypeKind::Enum);
    }
    return error_type;
}
TypeId PerModuleSema::lowerForeignConstantType(const cinterop::Constant &constant) {
    switch (constant.kind) {
    case cinterop::ConstantKind::Integer:
        return type_table.internInteger({constant.bits, constant.isSigned});
    case cinterop::ConstantKind::Float:
        return type_table.internFloat({constant.bits});
    case cinterop::ConstantKind::Bool:
        return bool_type;
    case cinterop::ConstantKind::Char:
        return char_type;
    }
    return error_type;
}
TypeId PerModuleSema::instantiateTypeExpr(frontend::TextSpan span, std::string_view name,
                                          const std::vector<frontend::TypeExprId> &arguments) {
    if (name == "Self") {
        report(span, "'Self' cannot be used with generic type arguments",
               diagnostics::err::UndefinedIdent);
        return error_type;
    }
    // The generic arguments are written in this (caller) module, so they are
    // lowered here against this module's snapshot. The template body itself is
    // reified in its declaring module below.
    session::ModuleKey declaring_module{};
    const frontend::Declaration *template_decl =
        findGenericTypeTemplate(name, &declaring_module);
    if (template_decl == nullptr) {
        report(span, "unknown generic type template '" + std::string(name) + "'",
               diagnostics::err::UndefinedIdent);
        return error_type;
    }
    if (arguments.size() != template_decl->genericParams.size()) {
        report(span, "wrong generic argument count for '" + std::string(name) + "'",
               diagnostics::err::GenericArity);
        return error_type;
    }
    std::vector<TypeId> args;
    args.reserve(arguments.size());
    for (const auto arg : arguments) {
        const TypeId lowered = lowerTypeExpr(arg);
        if (!lowered) {
            report(span, "generic argument is not a concrete type",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        }
        args.push_back(lowered);
    }
    // Reify the concrete type in the module that declares the template, so the
    // template body's own type expressions and name lookups resolve there. A
    // local template keeps using `this`.
    if (declaring_module != module && owner != nullptr) {
        if (PerModuleSema *declaring_sema = owner->findModuleSema(declaring_module))
            return declaring_sema->instantiateTemplateFromArgs(span, *template_decl, args);
    }
    return instantiateTemplateFromArgs(span, *template_decl, args);
}
TypeId PerModuleSema::instantiateTemplateFromArgs(frontend::TextSpan span,
                                                  const frontend::Declaration &template_decl,
                                                  const std::vector<TypeId> &args) {
    if (template_decl.kind != frontend::DeclKind::Struct &&
        template_decl.kind != frontend::DeclKind::Enum &&
        template_decl.kind != frontend::DeclKind::Union &&
        template_decl.kind != frontend::DeclKind::TypeAlias) {
        report(span, "'" + template_decl.name + "' is not a generic type that can be used here",
               diagnostics::err::GenericCannotInfer);
        return error_type;
    }
    const size_t arity = template_decl.genericParams.size();
    if (args.size() != arity) {
        report(span, "wrong generic argument count for '" + template_decl.name + "'",
               diagnostics::err::GenericArity);
        return error_type;
    }

    std::string concrete_name = std::string(template_decl.name) + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0)
            concrete_name += ",";
        concrete_name += typeArgumentName(args[i]);
    }
    if (args.empty())
        concrete_name += "?";
    concrete_name += ">";
    if (const TypeId existing = type_table.lookupReifiedStruct(concrete_name, args))
        return existing;

    // The template body is lowered in the module that declares the template.
    // `importedTemplateModule_` is set by the qualified struct-literal path; when
    // the caller routed here through `instantiateTypeExpr` this sema is already
    // the declaring module.
    PerModuleSema *field_sema = this;
    if (!importedTemplateModule_.empty() && importedTemplateModule_ != module &&
        owner != nullptr) {
        if (PerModuleSema *declaring_sema = owner->findModuleSema(importedTemplateModule_))
            field_sema = declaring_sema;
    }
    const bool imported_body       = field_sema != this;
    auto &active_storage = imported_body ? field_sema->activeTemplateArgs_ : activeTemplateArgs_;
    const std::vector<GenericBinding> saved_active = std::move(active_storage);
    active_storage.clear();
    for (size_t i = 0; i < template_decl.genericParams.size(); ++i) {
        GenericBinding active_binding;
        active_binding.name = template_decl.genericParams[i].name;
        active_binding.type = args[i];
        active_storage.push_back(std::move(active_binding));
    }
    const auto lower_body_type = [&](frontend::TypeExprId id) -> TypeId {
        return imported_body ? field_sema->lowerTypeExpr(id) : lowerTypeExpr(id);
    };

    TypeId concrete = error_type;
    switch (template_decl.kind) {
    case frontend::DeclKind::Struct: {
        auto &fields      = type_table.makeTypeStorage();
        auto &field_names = type_table.makeStringStorage();
        auto &field_meta  = type_table.makeFieldMetaStorage();
        for (const auto &param : template_decl.parameters) {
            const TypeId field_type = lower_body_type(param.type);
            fields.push(field_type ? field_type : error_type);
            field_meta.push(FieldMeta{param.visibility, param.modDepth, field_sema->module});
            char *buf = static_cast<char *>(field_sema->arena.alloc(param.name.size(), 1));
            std::memcpy(buf, param.name.data(), param.name.size());
            field_names.push(std::string_view(buf, param.name.size()));
        }
        const TypeId st =
            type_table.internStruct(concrete_name, fields, &field_names, &field_meta, &args,
                                    field_sema->requestedStructAlignment(template_decl),
                                    template_decl.isComponent);
        if (template_decl.isComponent)
            field_sema->checkComponentFields(template_decl, st, false);
        const std::string_view defining_module =
            type_table.definingModule(type_table.lookupNamed(template_decl.name));
        type_table.setDefiningModule(st, defining_module);
        type_table.registerNamed(concrete_name, st);
        concrete = st;
        break;
    }
    case frontend::DeclKind::Enum: {
        auto &variant_names = type_table.makeStringStorage();
        auto &discs         = type_table.makeDiscStorage();
        int64_t next        = 0;
        for (const auto &variant : template_decl.parameters) {
            char *buf = static_cast<char *>(field_sema->arena.alloc(variant.name.size(), 1));
            std::memcpy(buf, variant.name.data(), variant.name.size());
            variant_names.push(std::string_view(buf, variant.name.size()));
            discs.push(next++);
        }
        TypeId underlying =
            template_decl.declaredType ? lower_body_type(template_decl.declaredType) : i32_type;
        if (type_table.kindOf(resolve(underlying)) != TypeKind::Integer)
            underlying = i32_type;
        TypeId et = type_table.internEnum(concrete_name, underlying, variant_names, discs);
        type_table.setDefiningModule(et, type_table.definingModule(type_table.lookupNamed(
                                             template_decl.name)));
        type_table.registerNamed(concrete_name, et);
        concrete = et;
        break;
    }
    case frontend::DeclKind::Union: {
        auto &members = type_table.makeTypeStorage();
        for (size_t index = 0; index < template_decl.parameters.size(); ++index) {
            const auto &member_param = template_decl.parameters[index];
            TypeId member = lower_body_type(member_param.type);
            if (!member) {
                members.push(error_type);
                continue;
            }
            // A substituted member can be `void`/`never` even though the
            // template's own member is a generic parameter. A `void` member
            // gives the union no layout and makes LLVM codegen loop in
            // `getTypeAllocSize`; reject it here like the declaration path.
            const TypeKind member_kind = type_table.kindOf(resolve(member));
            if (member_kind == TypeKind::Void || member_kind == TypeKind::Never) {
                report(span,
                       "union member cannot be '" +
                           std::string(member_kind == TypeKind::Void ? "void" : "never") + "'",
                       diagnostics::err::TypeMismatch);
                reportNote(member_param.span,
                           "this generic member resolves to '" +
                               std::string(member_kind == TypeKind::Void ? "void" : "never") +
                               "' for these type arguments");
                members.push(error_type);
                continue;
            }
            members.push(member);
        }
        TypeId ut = type_table.internUnion(concrete_name, members, !template_decl.isRawUnion);
        type_table.setDefiningModule(ut, type_table.definingModule(type_table.lookupNamed(
                                             template_decl.name)));
        type_table.registerNamed(concrete_name, ut);
        concrete = ut;
        break;
    }
    case frontend::DeclKind::TypeAlias: {
        TypeId target = lower_body_type(template_decl.declaredType);
        if (!target) {
            concrete = error_type;
            break;
        }
        const TypeId substituted =
            instantiations != nullptr ? instantiations->substituteType(target, args) : target;
        TypeId alias = template_decl.isNominalType
                           ? type_table.internNominal(concrete_name, substituted)
                           : type_table.internAlias(concrete_name, substituted);
        type_table.registerNamed(concrete_name, alias);
        concrete = alias;
        break;
    }
    default:
        break;
    }

    active_storage.clear();
    for (auto &binding : saved_active)
        active_storage.push_back(std::move(binding));
    return concrete;
}

const frontend::Declaration *
PerModuleSema::findGenericTypeTemplate(std::string_view name,
                                       session::ModuleKey *out_module) const {
    if (out_module != nullptr)
        *out_module = {};
    const auto matches = [&](const frontend::Declaration &decl) {
        return decl.name == name && !decl.genericParams.empty() &&
               (decl.kind == frontend::DeclKind::Struct ||
                decl.kind == frontend::DeclKind::Enum ||
                decl.kind == frontend::DeclKind::Union ||
                decl.kind == frontend::DeclKind::TypeAlias);
    };
    // Local templates win so a module's own declaration is never shadowed by an
    // import of the same name.
    for (const auto &decl : snapshot.declarations()) {
        if (matches(decl)) {
            if (out_module != nullptr)
                *out_module = module;
            return &decl;
        }
    }
    if (owner == nullptr)
        return nullptr;
    // A qualified struct literal records its target module before instantiating.
    if (!importedTemplateModule_.empty() && importedTemplateModule_ != module) {
        if (const PerModuleSema *declaring = owner->findModuleSema(importedTemplateModule_)) {
            for (const auto &decl : declaring->snapshot.declarations()) {
                if (matches(decl)) {
                    if (out_module != nullptr)
                        *out_module = importedTemplateModule_;
                    return &decl;
                }
            }
        }
    }
    // `from module` imports bind each public symbol as an Import resolution.
    const auto *binding =
        session::lookupBinding(resolution, name, frontend::ScopeId{}, snapshot.scopes());
    if (binding != nullptr && binding->kind == session::ResolutionKind::Import &&
        !binding->target.module.empty()) {
        if (const PerModuleSema *target = owner->findModuleSema(binding->target.module)) {
            for (const auto &decl : target->snapshot.declarations()) {
                if (matches(decl)) {
                    if (out_module != nullptr)
                        *out_module = binding->target.module;
                    return &decl;
                }
            }
        }
    }
    return nullptr;
}
TypeId PerModuleSema::lowerBareTypeExpr(const frontend::TypeExpression &type) {
    switch (type.kind) {
    case frontend::TypeExprKind::Name: {
        if (!type.segments.empty()) {
            const auto target_module = resolveQualifiedPath(type);
            if (target_module.empty() || owner == nullptr)
                break;
            const auto *artifact = owner->findModuleSema(target_module);
            if (artifact == nullptr)
                break;

            const std::string_view symbol_name = type.segments.back();
            for (const auto &symbol : artifact->snapshot.declarations()) {
                if (symbol.name != symbol_name || symbol.visibility != frontend::Visibility::Public)
                    continue;
                if (symbol.kind == frontend::DeclKind::Enum ||
                    symbol.kind == frontend::DeclKind::Union) {
                    if (type.arguments.empty() && !symbol.genericParams.empty())
                        continue;
                }
                const bool is_named_type = symbol.kind == frontend::DeclKind::Struct ||
                                           symbol.kind == frontend::DeclKind::Enum ||
                                           symbol.kind == frontend::DeclKind::Union ||
                                           symbol.kind == frontend::DeclKind::Trait ||
                                           symbol.kind == frontend::DeclKind::Interface ||
                                           symbol.kind == frontend::DeclKind::TypeAlias;
                if (!is_named_type || !type.arguments.empty())
                    continue;
                if (const TypeId declared = artifact->typeOfDecl(symbol.id))
                    return declared;
                if (symbol.kind != frontend::DeclKind::TypeAlias) {
                    if (const TypeId named = type_table.lookupNamed(symbol.name))
                        return named;
                }
            }
            if (!type.arguments.empty()) {
                for (const auto &symbol : artifact->snapshot.declarations()) {
                    if (symbol.name != symbol_name ||
                        symbol.visibility != frontend::Visibility::Public ||
                        symbol.genericParams.empty())
                        continue;
                    if (symbol.kind == frontend::DeclKind::Struct ||
                        symbol.kind == frontend::DeclKind::Enum ||
                        symbol.kind == frontend::DeclKind::Union ||
                        symbol.kind == frontend::DeclKind::TypeAlias) {
                        // The generic arguments are written here (caller module),
                        // so lower them against this snapshot; reify the concrete
                        // type in the declaring module.
                        std::vector<TypeId> lowered;
                        lowered.reserve(type.arguments.size());
                        for (const auto arg : type.arguments) {
                            const TypeId arg_type = lowerTypeExpr(arg);
                            if (!arg_type) {
                                report(type.span, "generic argument is not a concrete type",
                                       diagnostics::err::GenericCannotInfer);
                                return error_type;
                            }
                            lowered.push_back(arg_type);
                        }
                        return const_cast<PerModuleSema *>(artifact)->instantiateTemplateFromArgs(
                            type.span, symbol, lowered);
                    }
                }
            }
            report(type.span,
                   "qualified type '" + type.name + "' names no public type in its module",
                   diagnostics::err::UndefinedIdent);
            return error_type;
        }
        // Generic application `Name<A, B>`: instantiate the template declaration
        // with the lowered concrete arguments. Function templates are handled by
        // generic call lowering; this path covers named type templates.
        if (!type.arguments.empty())
            return instantiateTypeExpr(type.span, type.name, type.arguments);
        // In implementations (`implement Point as Sample`), `Self` refers to the
        // implemented owner, not to the trait name.
        if (type.name == "Self") {
            // In implementations (`implement Point as Sample`), `Self` refers to the
            // implemented owner while this declaration is lowered.
            if (currentDeclId_ != 0U && currentDeclId_ <= snapshot.declarations().size()) {
                const auto &decl = snapshot.declarations()[currentDeclId_ - 1U];
                if (decl.ownerName.empty()) {
                    report(type.span, "'Self' is only valid inside an implementation",
                           diagnostics::err::UndefinedIdent);
                    return error_type;
                }
                if (const TypeId owner_type = type_table.lookupNamed(decl.ownerName))
                    return owner_type;
                report(type.span, "owner type '" + decl.ownerName + "' is not defined",
                       diagnostics::err::UndefinedIdent);
                return error_type;
            }
        }
        // When instantiating a template, template parameter names resolve to
        // the concrete arguments before anything else is considered. This must
        // win over `genericParams_` below: reification runs on the declaring
        // module's sema, whose `currentDeclId_` may still point at the template
        // declaration, so the stale `T -> GenericParam` binding would otherwise
        // shadow the concrete argument and leave the reified field as `T`.
        if (!activeTemplateArgs_.empty()) {
            for (const auto &binding : activeTemplateArgs_) {
                if (binding.name == type.name)
                    return binding.type;
            }
        }
        // A generic parameter name inside its own declaration resolves to an opaque
        // GenericParam type; the comptime solver rejects its use at instantiation.
        if (currentDeclId_ != 0U) {
            if (const auto *it = genericParams_.get(currentDeclId_)) {
                for (const auto &binding : *it) {
                    if (binding.name == type.name)
                        return binding.type;
                }
            }
        }
        // A generic named type must either be instantiated explicitly here or
        // appear inside a template that supplies its arguments through
        // `activeTemplateArgs_`. The registered placeholder returned below
        // would otherwise accept `Status`/`Union<...>` and lose the arity.
        const bool inactive_template =
            currentDeclId_ == 0U || !genericParams_.contains(currentDeclId_);
        for (const auto &decl : snapshot.declarations()) {
            if (decl.name != type.name || decl.genericParams.empty())
                continue;
            if (decl.kind != frontend::DeclKind::Enum && decl.kind != frontend::DeclKind::Union)
                continue;
            if (!inactive_template)
                continue;
            report(type.span, "wrong generic argument count for '" + type.name + "'",
                   diagnostics::err::GenericArity);
            return error_type;
        }
        // Unknown type names are an error: inventing a placeholder here used to
        // make every misspelled or unregistered type silently compatible with
        // anything. Before giving up, resolve a bare `from` import so a
        // consumer can name a struct, trait, or alias from the imported module.
        if (const TypeId named = resolvedTypeName(type.name))
            return named;
        if (const auto *resolved = findResolvedBinding(type.name, currentScopeForType(type));
            resolved != nullptr && resolved->foreignTypedef != nullptr) {
            return lowerForeignType(resolved->foreignTypedef->type);
        }
        if (const auto *resolved = findResolvedBinding(type.name, currentScopeForType(type));
            resolved != nullptr && resolved->kind == session::ResolutionKind::Import) {
            if (const TypeId imported = typeOfResolvedBinding(*resolved))
                return imported;
        }
        report(type.span, "unknown type '" + type.name + "'", diagnostics::err::UndefinedIdent);
        return error_type;
    }
    case frontend::TypeExprKind::Pointer:
    case frontend::TypeExprKind::Optional: {
        // `*void` / `?*void` are rejected; `raw opaque` remains the spelling
        // for C interop. ?*T nullable pointers stay valid.
        TypeId inner = type.arguments.empty() ? kInvalidTypeId : lowerTypeExpr(type.arguments[0]);
        if (inner) {
            const TypeId stripped       = type_table.stripQualifiers(inner);
            const TypeId resolved_inner = resolve(stripped);
            if (resolved_inner == void_type) {
                report(type.span,
                       "pointer to 'void' is not allowed; use 'raw opaque' for C interop",
                       diagnostics::err::TypeMismatch);
                return error_type;
            }
        }
        return type.kind == frontend::TypeExprKind::Pointer ? type_table.internPointer(inner)
                                                            : type_table.internOptional(inner);
    }
    case frontend::TypeExprKind::Array:
        return type_table.internArray(type.arguments.empty() ? kInvalidTypeId
                                                             : lowerTypeExpr(type.arguments[0]),
                                      type.arrayLength);
    case frontend::TypeExprKind::Slice:
        // `[...]T` and `[]T` share the same runtime slice type. The parser
        // records `isVariadicSlice` on the declaration so call resolution can
        // collect extra homogeneous arguments into the slice at the call site.
        return type_table.internSlice(type.arguments.empty() ? kInvalidTypeId
                                                             : lowerTypeExpr(type.arguments[0]));
    case frontend::TypeExprKind::Function: {
        auto &params = type_table.makeTypeStorage();
        for (size_t i = 0; i + 1 < type.arguments.size(); ++i)
            params.push(lowerTypeExpr(type.arguments[i]));
        const TypeId result =
            type.arguments.empty() ? kInvalidTypeId : lowerTypeExpr(type.arguments.back());
        return type.isStateFunctionType ? type_table.internStateFunction(params, result)
                                        : type_table.internFunction(params, result);
    }
    case frontend::TypeExprKind::Opaque:
        // `raw opaque` is the only accepted spelling of an untyped pointer; a literally
        // written `*void` is still rejected above, via TypeExprKind::Pointer.
        return type_table.internPointer(void_type);
    case frontend::TypeExprKind::OpaqueTagged:
        return opaque_type;
    case frontend::TypeExprKind::Parenthesized:
        return type.arguments.empty() ? error_type : lowerTypeExpr(type.arguments[0]);
    case frontend::TypeExprKind::Pack: {
        if (type.member_names.size() != type.arguments.size()) {
            report(type.span, "pack type members must be named", diagnostics::err::TypeMismatch);
            return error_type;
        }
        auto &members = type_table.makeTypeStorage();
        auto &names   = type_table.makeStringStorage();
        for (const auto &arg : type.arguments)
            members.push(lowerTypeExpr(arg));
        for (const auto &name : type.member_names)
            names.push(name);
        for (size_t i = 0; i < names.size(); ++i)
            for (size_t j = i + 1U; j < names.size(); ++j)
                if (names[i] == names[j]) {
                    report(type.span, "duplicate pack member name '" + std::string(names[i]) + "'",
                           diagnostics::err::TypeMismatch);
                    return error_type;
                }
        return type_table.internPack(members, names);
    }
    case frontend::TypeExprKind::Dyn: {
        if (type.arguments.empty())
            return error_type;
        TypeId target                       = kInvalidTypeId;
        const frontend::TypeExprId inner_id = type.arguments[0];
        if (inner_id && inner_id.value <= snapshot.typeExpressions().size()) {
            const auto &inner_expr = snapshot.typeExpressions()[inner_id.value - 1U];
            if (inner_expr.kind == frontend::TypeExprKind::Name && inner_expr.segments.empty()) {
                const TypeId resolved = resolvedTraitType(inner_expr.name);
                target                = resolved ? resolved : lowerTypeExpr(inner_id);
            } else {
                target = lowerTypeExpr(inner_id);
            }
        } else {
            target = error_type;
        }
        target         = resolve(target);
        const auto *tr = type_table.trait(target);
        if (tr == nullptr) {
            report(type.span, "'dyn' target must be a trait or interface",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        session::ModuleKey trait_module{};
        const frontend::Declaration *decl =
            findDeclNamed(tr->name,
                          findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr
                              ? frontend::DeclKind::Interface
                              : frontend::DeclKind::Trait,
                          &trait_module);
        const bool is_iface     = findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr;
        size_t methods          = 0;
        const auto countMethods = [&](const frontend::FrontendSnapshot &snap) {
            for (const auto &candidate : snap.declarations()) {
                if (candidate.kind == frontend::DeclKind::Function &&
                    candidate.ownerName == tr->name && candidate.name != "self")
                    ++methods;
            }
        };
        if (decl != nullptr && !trait_module.empty()) {
            if (trait_module == module) {
                countMethods(snapshot);
            } else if (owner != nullptr) {
                const auto *trait_sema = owner->findModuleSema(trait_module);
                if (trait_sema != nullptr)
                    countMethods(trait_sema->snapshot);
            }
        }
        if (methods == 0) {
            report(type.span,
                   is_iface ? "'dyn Interface' requires at least one method requirement"
                            : "'dyn Trait' requires at least one method requirement",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        return type_table.internDyn(target, methods);
    }
    case frontend::TypeExprKind::Error:
        return kInvalidTypeId;
    }
    return kInvalidTypeId;
}

} // namespace zith::sema::modern
