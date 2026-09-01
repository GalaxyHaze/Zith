#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <string>

namespace zith::sema::modern {

bool PerModuleSema::isConstantExpression(frontend::ExprId id) const {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    switch (expr.kind) {
    case frontend::ExprKind::Literal:
        return true;
    case frontend::ExprKind::ArrayLiteral:
        for (const auto operand : expr.operands)
            if (!isConstantExpression(operand))
                return false;
        return true;
    case frontend::ExprKind::StructLiteral:
        for (const auto operand : expr.operands)
            if (!isConstantExpression(operand))
                return false;
        return true;
    case frontend::ExprKind::Name: {
        const auto *resolved = findResolvedExpr(id);
        if (resolved == nullptr)
            return false;
        if (resolved->foreignConstant != nullptr)
            return resolved->bindingKind == frontend::BindingKind::Const;
        if (resolved->local)
            return resolved->bindingKind == frontend::BindingKind::Const;
        if (resolved->kind == session::ResolutionKind::Import) {
            if (!resolved->target.localSymbol)
                return false;
            const auto *decl =
                resolved->target.localSymbol.value <= snapshot.declarations().size()
                    ? &snapshot.declarations()[resolved->target.localSymbol.value - 1U]
                    : nullptr;
            return decl != nullptr && decl->kind == frontend::DeclKind::Variable &&
                   decl->bindingKind == frontend::BindingKind::Const;
        }
        if (resolved->declaration) {
            const auto *decl = resolved->declaration.value <= snapshot.declarations().size()
                                   ? &snapshot.declarations()[resolved->declaration.value - 1U]
                                   : nullptr;
            return decl != nullptr && decl->kind == frontend::DeclKind::Variable &&
                   decl->bindingKind == frontend::BindingKind::Const;
        }
        if (resolved->target.module.empty() && resolved->target.localSymbol)
            return false;
        return false;
    }
    default:
        return false;
    }
}
bool PerModuleSema::targetFieldIsConst(frontend::ExprId id) const {
    for (unsigned guard = 0; guard < 64U && id && id.value <= snapshot.expressions().size();
         ++guard) {
        const auto &expr = snapshot.expressions()[id.value - 1U];
        if (expr.kind != frontend::ExprKind::Field && expr.kind != frontend::ExprKind::Arrow)
            return false;
        if (expr.operands.empty())
            return false;

        TypeId object_type = typeOfExpr(expr.operands[0]);
        if (!object_type)
            return false;
        TypeId object = type_table.stripQualifiers(object_type);
        if (expr.kind == frontend::ExprKind::Arrow) {
            const TypeId pointer = pointerBase(object);
            if (!pointer)
                return false;
            const auto *ptr = type_table.pointer(pointer);
            object = type_table.stripQualifiers(ptr != nullptr ? ptr->pointee : kInvalidTypeId);
            if (!object)
                return false;
        }

        const auto *struct_t = type_table.struct_type(object);
        if (struct_t != nullptr) {
            const auto idx = type_table.fieldIndex(object, expr.text);
            if (idx >= 0 && findConstField(struct_t->name, static_cast<size_t>(idx)))
                return true;
        }
        // A const field can be nested through ordinary fields, so continue
        // walking the base expression (`p.a.b` where `a` is const).
        id = expr.operands[0];
    }
    return false;
}
bool PerModuleSema::findConstField(std::string_view struct_name, size_t index) const noexcept {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Struct || decl.name != struct_name)
            continue;
        if (index < decl.parameters.size())
            return decl.parameters[index].isConstField;
        break;
    }
    return false;
}
void PerModuleSema::checkZithDeclarations() {
    // Unknown `as`/`for` targets are deferred from the parser to sema so the
    // frontend can still represent cross-module implementations before imports
    // are resolved. Unknown targets are not nominal traits or interfaces.
    for (const auto &record : snapshot.implementRecords()) {
        if (findDeclNamed(record.traitName, frontend::DeclKind::Trait) == nullptr &&
            findDeclNamed(record.traitName, frontend::DeclKind::Interface) == nullptr) {
            report(record.span, "'" + record.traitName + "' is not a declared trait or interface",
                   diagnostics::err::NotATrait);
        }
    }

    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Variable)
            continue;
        TypeId declared = typeOfDecl(decl.id);
        if (!declared)
            declared = error_type;
        const TypeId stripped = type_table.stripQualifiers(declared);
        const TypeKind kind   = stripped ? type_table.kindOf(stripped) : TypeKind::Error;
        if (decl.bindingKind == frontend::BindingKind::Const) {
            if (!decl.initializer) {
                report(decl.span, "Zith--: const declaration requires an initializer",
                       diagnostics::err::UnsupportedSyntax);
            } else if (!isConstantExpression(decl.initializer)) {
                report(snapshot.expressions()[decl.initializer.value - 1U].span,
                       "Zith--: const initializer must be a constant expression",
                       diagnostics::err::UnsupportedSyntax);
            }
        } else if (decl.declaredType && kind != TypeKind::Integer && kind != TypeKind::Float &&
                   kind != TypeKind::Bool && kind != TypeKind::Char && kind != TypeKind::Void &&
                   !decl.initializer) {
            report(decl.span, "Zith--: non-trivial let/var declaration requires an initializer",
                   diagnostics::err::UnsupportedSyntax);
        }
    }

    std::vector<frontend::LocalId> for_in_bindings;
    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind == frontend::ExprKind::ForIn && expr.forInBinding)
            for_in_bindings.push_back(expr.forInBinding);
    }
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind != frontend::StmtKind::Binding)
            continue;
        const auto &binding = statement.binding;
        if (binding.bindingKind == frontend::BindingKind::Const && !binding.initializer) {
            report(binding.span, "Zith--: const binding requires an initializer",
                   diagnostics::err::UnsupportedSyntax);
            continue;
        }
        if (binding.bindingKind == frontend::BindingKind::Const &&
            !isConstantExpression(binding.initializer)) {
            report(binding.span, "Zith--: const binding initializer must be a constant expression",
                   diagnostics::err::UnsupportedSyntax);
            continue;
        }
        TypeId local_type = typeOfLocal(binding.id);
        if (!local_type)
            continue;
        const TypeId stripped = type_table.stripQualifiers(local_type);
        const TypeKind kind   = stripped ? type_table.kindOf(stripped) : TypeKind::Error;
        const bool non_trivial =
            kind == TypeKind::Pointer || kind == TypeKind::Array || kind == TypeKind::Slice ||
            kind == TypeKind::Optional || kind == TypeKind::Struct || kind == TypeKind::Union ||
            kind == TypeKind::Enum || kind == TypeKind::String || kind == TypeKind::GenericParam ||
            kind == TypeKind::Incomplete || kind == TypeKind::Nominal || kind == TypeKind::Alias ||
            kind == TypeKind::Function || kind == TypeKind::Failable || kind == TypeKind::Pack ||
            kind == TypeKind::Trait || kind == TypeKind::Sum || kind == TypeKind::TypeVar;
        const bool is_for_in_binding = std::find(for_in_bindings.begin(), for_in_bindings.end(),
                                                 binding.id) != for_in_bindings.end();
        if (!binding.initializer && !is_for_in_binding &&
            (binding.bindingKind == frontend::BindingKind::Let ||
             binding.bindingKind == frontend::BindingKind::Var) &&
            non_trivial) {
            report(binding.span, "Zith--: non-trivial let/var binding requires an initializer",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
}
void PerModuleSema::checkConstFieldAssignments() {
    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind != frontend::ExprKind::Assign)
            continue;
        if (targetFieldIsConst(expr.operands[0]))
            report(expr.span, "Zith--: cannot assign to a const struct field",
                   diagnostics::err::UnsupportedSyntax);
    }
}
bool PerModuleSema::allowsUncheckedNullablePointer(TypeId target, TypeId source) const noexcept {
    // TEMPORARY: every C pointer is `?*T`, but flow-sensitive narrowing after `is null`
    // does not exist yet, so a nullable pointer is accepted wherever `*T` is expected.
    // This is the single removal point: once narrowing (and/or `must`/`raw`) lands, delete
    // this predicate and unchecked use becomes a diagnostic. See docs/08-error-handling.md.
    if (type_table.kindOf(resolve(target)) != TypeKind::Pointer)
        return false;
    const TypeId resolved_source = resolve(source);
    if (type_table.kindOf(resolved_source) != TypeKind::Optional)
        return false;
    const auto *opt = type_table.optional(resolved_source);
    return opt != nullptr && type_table.kindOf(resolve(opt->inner)) == TypeKind::Pointer &&
           sameType(target, opt->inner);
}
bool PerModuleSema::coercesTo(TypeId target, TypeId source) const noexcept {
    bool result = false;
    if (sameType(target, source)) {
        result = true;
    } else {
        const TypeId resolved_target = resolve(target);
        const TypeId resolved_source = resolve(source);
        // Generic enum templates are constant types: a value of `Status.Ok`
        // can be stored in or passed to any concrete `Status<T>` instance.
        // The concrete instance keeps the template variant set/discriminants.
        if (type_table.kindOf(resolved_target) == TypeKind::Enum &&
            type_table.kindOf(resolved_source) == TypeKind::Enum) {
            const auto *target_enum = type_table.enum_type(resolved_target);
            const auto *source_enum = type_table.enum_type(resolved_source);
            const auto baseName     = [](std::string_view name) {
                if (const size_t angle = name.find('<'); angle != std::string_view::npos)
                    return std::string_view(name.data(), angle);
                return name;
            };
            result = target_enum != nullptr && source_enum != nullptr &&
                     baseName(target_enum->name) == baseName(source_enum->name);
        } else if (type_table.kindOf(resolved_target) == TypeKind::Dyn) {
            const auto *dyn = type_table.dyn_type(resolved_target);
            if (dyn != nullptr) {
                if (type_table.kindOf(resolved_source) == TypeKind::Dyn) {
                    result = sameType(resolved_target, resolved_source);
                } else {
                    const TypeKind source_kind = type_table.kindOf(resolved_source);
                    result = satisfiesConformance(resolved_source, resolve(dyn->target)) &&
                             (type_table.struct_type(resolved_source) != nullptr ||
                              type_table.slice(resolved_source) != nullptr ||
                              source_kind == TypeKind::Enum || source_kind == TypeKind::Union ||
                              source_kind == TypeKind::Integer || source_kind == TypeKind::Float ||
                              source_kind == TypeKind::Bool || source_kind == TypeKind::Char ||
                              source_kind == TypeKind::Pointer);
                }
            }
        } else if (type_table.kindOf(resolved_target) == TypeKind::Optional) {
            if (resolved_source == null_type) {
                result = true;
            } else if (const auto *opt = type_table.optional(resolved_target)) {
                // `??T` accepts `?T`, and `?T` accepts `T`: optional target
                // types add missing layers at any depth, so a bare `T` can be
                // wrapped by successively smaller targets. The reverse case
                // (discarding an outer optional) still requires an explicit
                // unwrap and is not accepted here.
                result = sameType(opt->inner, source) || coercesTo(opt->inner, source);
            }
        } else {
            result = allowsUncheckedNullablePointer(target, source);
        }
        // Any pointer (or nullable pointer) reaches a `void*` parameter without a cast, so
        // `free(x)` works for both `*i32` and `?*i32`. The reverse still needs `as`.
        if (!result && isVoidPointer(resolved_target) && pointerBase(source))
            result = true;
        // A fixed array is an implicit view into a slice of the same element type.
        if (!result && type_table.kindOf(resolved_target) == TypeKind::Slice) {
            const auto *slice = type_table.slice(resolved_target);
            const auto *array = type_table.array(resolved_source);
            result =
                slice != nullptr && array != nullptr && sameType(slice->element, array->element);
        }
        // A `[]char` slice may be viewed as its storage pointer. The applied
        // coercion records the escape in `coerceValue`; this type-level probe
        // intentionally stays generic for overload scoring.
        if (!result && type_table.kindOf(resolved_target) == TypeKind::Pointer) {
            const auto *ptr   = type_table.pointer(resolved_target);
            const auto *slice = type_table.slice(resolved_source);
            result            = ptr != nullptr && slice != nullptr &&
                     sameType(type_table.stripQualifiers(ptr->pointee), char_type) &&
                     sameType(type_table.stripQualifiers(slice->element), char_type);
        }
        // A positional pack literal coerces to a named pack when member types
        // and arity match. Sema keeps the literal's empty name list so the
        // binding's annotation supplies the field names to HIR.
        if (!result && type_table.kindOf(resolved_target) == TypeKind::Pack) {
            const auto *target_pack = type_table.pack(resolved_target);
            const auto *source_pack = type_table.pack(resolved_source);
            if (target_pack != nullptr && source_pack != nullptr &&
                target_pack->members.size() == source_pack->members.size()) {
                result = true;
                for (size_t index = 0; index < target_pack->members.size(); ++index) {
                    if (!sameType(target_pack->members[index], source_pack->members[index])) {
                        result = false;
                        break;
                    }
                }
            }
        }
    }
    return result;
}
bool PerModuleSema::variadicFinalArgIsExplicitSlice(TypeId slice_type,
                                                    const std::vector<frontend::ExprId> &args,
                                                    size_t fixed_explicit_args) const {
    // `args` is the full call operand vector: `args[0]` is the callee or
    // receiver expression, and the arguments that can be collected begin at
    // `fixed_explicit_args + 1`.
    if (args.size() != fixed_explicit_args + 2U)
        return false;
    // Prefer already-inferred types: `args.back()` may be a later expression
    // in the same call, so re-inferring it here can regress typed_map state.
    const TypeId last_type = typeOfExpr(args.back());
    if (!last_type)
        return false;
    const TypeId last = resolve(last_type);
    if (type_table.slice(last) == nullptr && type_table.array(last) == nullptr)
        return false;
    const auto *slice = type_table.slice(resolve(slice_type));
    if (slice == nullptr)
        return false;
    const TypeId element = resolve(slice->element);
    if (type_table.kindOf(element) == TypeKind::Dyn) {
        // A homogeneous `[]dyn Trait` final argument remains explicit. A
        // concrete final slice still must be erased, so it is auto-collected
        // unless it is already a dyn slice.
        const auto *last_slice = type_table.slice(last);
        return last_slice != nullptr &&
               type_table.kindOf(resolve(last_slice->element)) == TypeKind::Dyn;
    }
    return true;
}
bool PerModuleSema::unify(TypeId expected, TypeId actual) {
    return sameType(expected, actual);
}
bool PerModuleSema::sameType(TypeId a, TypeId b) const noexcept {
    if (a == b)
        return true;
    const auto resolved_a = resolve(a);
    const auto resolved_b = resolve(b);
    if (resolved_a == resolved_b)
        return true;
    TypeKind ka = type_table.kindOf(resolved_a);
    TypeKind kb = type_table.kindOf(resolved_b);
    if (resolved_a == null_type || resolved_b == null_type)
        return ka == TypeKind::Optional || kb == TypeKind::Optional;
    // `error` suppresses cascading diagnostics; `Unknown` (generics / type vars) does not.
    if (resolved_a == error_type || resolved_b == error_type)
        return true;
    const bool state_or_fn_shape = (ka == TypeKind::State && kb == TypeKind::Function) ||
                                   (ka == TypeKind::Function && kb == TypeKind::State);
    if (ka != kb && !state_or_fn_shape)
        return false;
    if (ka == TypeKind::Integer) {
        const auto *ia = type_table.integer(resolved_a);
        const auto *ib = type_table.integer(resolved_b);
        return ia && ib && ia->bits == ib->bits && ia->isSigned == ib->isSigned;
    }
    if (ka == TypeKind::Float) {
        const auto *fa = type_table.float_kind(resolved_a);
        const auto *fb = type_table.float_kind(resolved_b);
        return fa && fb && fa->bits == fb->bits;
    }
    if (ka == TypeKind::Dyn) {
        const auto *da = type_table.dyn_type(resolved_a);
        const auto *db = type_table.dyn_type(resolved_b);
        return da != nullptr && db != nullptr && da->method_count == db->method_count &&
               sameType(da->target, db->target);
    }
    if (ka == TypeKind::Void || ka == TypeKind::Never || ka == TypeKind::Bool ||
        ka == TypeKind::Char || ka == TypeKind::String) {
        return true;
    }
    if (ka == TypeKind::Pointer) {
        const auto *pa = type_table.pointer(resolved_a);
        const auto *pb = type_table.pointer(resolved_b);
        return pa != nullptr && pb != nullptr && sameType(pa->pointee, pb->pointee);
    }
    if (ka == TypeKind::GenericParam) {
        uint32_t da = 0;
        uint32_t ia = 0;
        uint32_t db = 0;
        uint32_t ib = 0;
        type_table.genericParamOrigin(resolved_a, &da, &ia);
        type_table.genericParamOrigin(resolved_b, &db, &ib);
        // An implement-method `T` is the owner's generic parameter. It is
        // intentionally interned under the owner decl so it unifies with the
        // field type of `Owner<T>`.
        return da == db && ia == ib;
    }
    if (ka == TypeKind::Optional) {
        const auto *oa = type_table.optional(resolved_a);
        const auto *ob = type_table.optional(resolved_b);
        return oa != nullptr && ob != nullptr && sameType(oa->inner, ob->inner);
    }
    if (ka == TypeKind::Slice) {
        const auto *sa = type_table.slice(resolved_a);
        const auto *sb = type_table.slice(resolved_b);
        return sa != nullptr && sb != nullptr && sameType(sa->element, sb->element);
    }
    if (ka == TypeKind::Array) {
        const auto *aa = type_table.array(resolved_a);
        const auto *ab = type_table.array(resolved_b);
        return aa != nullptr && ab != nullptr && aa->size == ab->size &&
               sameType(aa->element, ab->element);
    }
    if (ka == TypeKind::Pack) {
        const auto *pa = type_table.pack(resolved_a);
        const auto *pb = type_table.pack(resolved_b);
        if (pa == nullptr || pb == nullptr || pa->members.size() != pb->members.size())
            return false;
        for (size_t index = 0; index < pa->members.size(); ++index) {
            if (!sameType(pa->members[index], pb->members[index]))
                return false;
        }
        return true;
    }
    if (ka == TypeKind::Function || ka == TypeKind::State || kb == TypeKind::Function ||
        kb == TypeKind::State) {
        const auto *fa = type_table.function(resolved_a);
        const auto *fb = type_table.function(resolved_b);
        if (fa == nullptr || fb == nullptr || fa->params.size() != fb->params.size() ||
            !sameType(fa->result, fb->result)) {
            return false;
        }
        for (size_t index = 0; index < fa->params.size(); ++index) {
            if (!sameType(fa->params[index], fb->params[index]))
                return false;
        }
        return true;
    }
    if (ka == TypeKind::Struct) {
        // Nominal identity: a forward-declared placeholder and the completed struct share a name.
        const auto *sa = type_table.struct_type(resolved_a);
        const auto *sb = type_table.struct_type(resolved_b);
        return sa != nullptr && sb != nullptr && sa->name == sb->name;
    }
    if (ka == TypeKind::Enum) {
        const auto *ea = type_table.enum_type(resolved_a);
        const auto *eb = type_table.enum_type(resolved_b);
        return ea != nullptr && eb != nullptr && ea->name == eb->name;
    }
    if (ka == TypeKind::Union) {
        const auto *ua = type_table.union_type(resolved_a);
        const auto *ub = type_table.union_type(resolved_b);
        return ua != nullptr && ub != nullptr && ua->name == ub->name;
    }
    if (ka == TypeKind::Alias) {
        const auto *alias_a = type_table.alias(resolved_a);
        const auto *alias_b = type_table.alias(resolved_b);
        return alias_a != nullptr && alias_b != nullptr &&
               sameType(alias_a->target, alias_b->target);
    }
    if (ka == TypeKind::Nominal) {
        const auto *na = type_table.nominal(resolved_a);
        const auto *nb = type_table.nominal(resolved_b);
        return na != nullptr && nb != nullptr && na->name == nb->name;
    }
    return false;
}
TypeId PerModuleSema::resolve(TypeId t) const noexcept {
    t = type_table.canonical(t);
    for (unsigned guard = 0; guard < 16U; ++guard) {
        if (const auto *alias = type_table.alias(t)) {
            t = type_table.canonical(alias->target);
            continue;
        }
        // Memory qualifiers are transparent to the type relations; only the
        // dedicated ownership checks inspect them directly.
        if (const auto *qual = type_table.qualified(t)) {
            t = type_table.canonical(qual->inner);
            continue;
        }
        break;
    }
    return t;
}
TypeId PerModuleSema::concreteBase(TypeId t) const noexcept {
    return t;
}

} // namespace zith::sema::modern
