#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <cstring>
#include <string>

namespace zith::sema::modern {

const frontend::Declaration *PerModuleSema::findDeclNamed(std::string_view name,
                                                          frontend::DeclKind kind) const {
    const auto findIn =
        [&](const frontend::FrontendSnapshot &snap) -> const frontend::Declaration * {
        for (const auto &decl : snap.declarations()) {
            if (decl.kind == kind && decl.name == name)
                return &decl;
        }
        return nullptr;
    };
    if (const auto *found = findIn(snapshot))
        return found;
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            if (const auto *found = findIn(*artifact.frontend))
                return found;
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
    const auto *binding = findResolvedBinding(type.segments.front(), currentScopeForType(type));
    if (binding == nullptr || binding->kind != session::ResolutionKind::ModuleAlias)
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
    const frontend::Declaration *template_decl = nullptr;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.name == name && !decl.genericParams.empty()) {
            template_decl = &decl;
            break;
        }
    }
    if (template_decl == nullptr) {
        report(span, "unknown generic type template '" + std::string(name) + "'",
               diagnostics::err::UndefinedIdent);
        return error_type;
    }
    const size_t arity = template_decl->genericParams.size();
    if (arguments.size() != arity) {
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
    std::string concrete_name = std::string(name) + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0)
            concrete_name += ",";
        concrete_name += type_table.typeToString(args[i]);
    }
    concrete_name += ">";
    if (const TypeId existing = type_table.lookupNamed(concrete_name))
        return existing;

    switch (template_decl->kind) {
    case frontend::DeclKind::Struct: {
        auto &fields                             = type_table.makeTypeStorage();
        auto &fld_names                          = type_table.makeStringStorage();
        auto &field_meta                         = type_table.makeFieldMetaStorage();
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
        for (const auto &param : template_decl->parameters) {
            TypeId ftype = lowerTypeExpr(param.type);
            fields.push(ftype ? ftype : error_type);
            field_meta.push(FieldMeta{param.visibility, param.modDepth, module});
            char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
            std::memcpy(buf, param.name.data(), param.name.size());
            fld_names.push(std::string_view(buf, param.name.size()));
        }
        activeTemplateArgs_ = std::move(saved_active);
        TypeId st = type_table.internStruct(concrete_name, fields, &fld_names, &field_meta);
        type_table.registerNamed(concrete_name, st);
        return st;
    }
    case frontend::DeclKind::Enum: {
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
        auto &variant_names = type_table.makeStringStorage();
        auto &discs         = type_table.makeDiscStorage();
        int64_t next        = 0;
        for (const auto &variant : template_decl->parameters) {
            char *buf = static_cast<char *>(arena.alloc(variant.name.size(), 1));
            std::memcpy(buf, variant.name.data(), variant.name.size());
            variant_names.push(std::string_view(buf, variant.name.size()));
            discs.push(next++);
        }
        TypeId underlying =
            template_decl->declaredType ? lowerTypeExpr(template_decl->declaredType) : i32_type;
        activeTemplateArgs_ = std::move(saved_active);
        if (type_table.kindOf(resolve(underlying)) != TypeKind::Integer)
            underlying = i32_type;
        TypeId et = type_table.internEnum(concrete_name, underlying, variant_names, discs);
        type_table.registerNamed(concrete_name, et);
        return et;
    }
    case frontend::DeclKind::Union: {
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
        auto &members = type_table.makeTypeStorage();
        for (const auto &member_param : template_decl->parameters) {
            TypeId member = lowerTypeExpr(member_param.type);
            members.push(member ? member : error_type);
        }
        activeTemplateArgs_ = std::move(saved_active);
        TypeId ut = type_table.internUnion(concrete_name, members, !template_decl->isRawUnion);
        type_table.registerNamed(concrete_name, ut);
        return ut;
    }
    case frontend::DeclKind::TypeAlias: {
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
        TypeId target       = lowerTypeExpr(template_decl->declaredType);
        activeTemplateArgs_ = std::move(saved_active);
        if (!target)
            return error_type;
        const TypeId substituted =
            instantiations != nullptr ? instantiations->substituteType(target, args) : target;
        TypeId alias = template_decl->isNominalType
                           ? type_table.internNominal(concrete_name, substituted)
                           : type_table.internAlias(concrete_name, substituted);
        type_table.registerNamed(concrete_name, alias);
        return alias;
    }
    default:
        report(span, "'" + std::string(name) + "' is not a generic type that can be used here",
               diagnostics::err::GenericCannotInfer);
        return error_type;
    }
}
TypeId PerModuleSema::instantiateStructFromArgs(frontend::TextSpan span,
                                                const frontend::Declaration &template_decl,
                                                const std::vector<TypeId> &args) {
    if (template_decl.kind != frontend::DeclKind::Struct)
        return error_type;
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
        concrete_name += type_table.typeToString(args[i]);
    }
    if (args.empty())
        concrete_name += "?";
    concrete_name += ">";
    if (const TypeId existing = type_table.lookupNamed(concrete_name))
        return existing;

    auto &fields                                   = type_table.makeTypeStorage();
    auto &field_names                              = type_table.makeStringStorage();
    auto &field_meta                               = type_table.makeFieldMetaStorage();
    const std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
    activeTemplateArgs_.clear();
    for (size_t i = 0; i < template_decl.genericParams.size(); ++i) {
        GenericBinding active_binding;
        active_binding.name = template_decl.genericParams[i].name;
        active_binding.type = args[i];
        activeTemplateArgs_.push_back(std::move(active_binding));
    }
    for (const auto &param : template_decl.parameters) {
        const TypeId field_type = lowerTypeExpr(param.type);
        fields.push(field_type ? field_type : error_type);
        field_meta.push(FieldMeta{param.visibility, param.modDepth, module});
        char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
        std::memcpy(buf, param.name.data(), param.name.size());
        field_names.push(std::string_view(buf, param.name.size()));
    }
    activeTemplateArgs_ = saved_active;

    const TypeId st = type_table.internStruct(concrete_name, fields, &field_names, &field_meta);
    type_table.registerNamed(concrete_name, st);
    return st;
}
TypeId PerModuleSema::lowerBareTypeExpr(const frontend::TypeExpression &type) {
    switch (type.kind) {
    case frontend::TypeExprKind::Name: {
        if (!type.segments.empty()) {
            const auto target_module = resolveQualifiedPath(type);
            if (target_module.empty())
                break;
            if (owner == nullptr || owner->findModuleSema(target_module) == nullptr)
                break;
            const auto *artifact               = owner->findModuleSema(target_module);
            const std::string_view symbol_name = type.segments.back();
            for (const auto &symbol : artifact->snapshot.declarations()) {
                if (symbol.name != symbol_name)
                    continue;
                if (symbol.kind == frontend::DeclKind::Enum ||
                    symbol.kind == frontend::DeclKind::Union) {
                    if (type.arguments.empty() && !symbol.genericParams.empty())
                        continue;
                    if (const TypeId named = type_table.lookupNamed(symbol.name))
                        return named;
                }
            }
            for (const auto &symbol : artifact->snapshot.declarations()) {
                if (symbol.name == symbol_name && symbol.kind == frontend::DeclKind::Struct) {
                    if (const TypeId named = type_table.lookupNamed(symbol.name))
                        return named;
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
        // A generic parameter name inside its own declaration resolves to an opaque
        // GenericParam type; the comptime solver rejects its use at instantiation.
        if (currentDeclId_ != 0U) {
            if (const auto it = genericParams_.find(currentDeclId_); it != genericParams_.end()) {
                for (const auto &binding : it->second) {
                    if (binding.name == type.name)
                        return binding.type;
                }
            }
        }
        // When instantiating a template, template parameter names resolve to
        // the concrete arguments before anything else is considered.
        if (!activeTemplateArgs_.empty()) {
            for (const auto &binding : activeTemplateArgs_) {
                if (binding.name == type.name)
                    return binding.type;
            }
        }
        // A generic named type must either be instantiated explicitly here or
        // appear inside a template that supplies its arguments through
        // `activeTemplateArgs_`. The registered placeholder returned below
        // would otherwise accept `Status`/`Union<...>` and lose the arity.
        const bool inactive_template =
            currentDeclId_ == 0U || genericParams_.find(currentDeclId_) == genericParams_.end();
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
        if (const TypeId named = type_table.lookupNamed(type.name))
            return named;
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
        const TypeId target = resolve(lowerTypeExpr(type.arguments[0]));
        const auto *tr      = type_table.trait(target);
        if (tr == nullptr) {
            report(type.span, "'dyn' target must be a trait or interface",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        const frontend::Declaration *decl = findDeclNamed(
            tr->name, findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr
                          ? frontend::DeclKind::Interface
                          : frontend::DeclKind::Trait);
        const bool is_iface = findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr;
        size_t methods      = 0;
        if (decl != nullptr) {
            for (const auto &candidate : snapshot.declarations()) {
                if (candidate.kind == frontend::DeclKind::Function &&
                    candidate.ownerName == tr->name && candidate.name != "self")
                    ++methods;
            }
            if (owner != nullptr) {
                for (const auto &artifact : owner->modules()) {
                    if (artifact->frontend == nullptr || artifact->key == module)
                        continue;
                    for (const auto &candidate : artifact->frontend->declarations()) {
                        if (candidate.kind == frontend::DeclKind::Function &&
                            candidate.ownerName == tr->name)
                            ++methods;
                    }
                }
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
