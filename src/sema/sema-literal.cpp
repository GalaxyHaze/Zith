#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <string>

namespace zith::sema::modern {

TypeId PerModuleSema::resolveGenericStructLiteral(frontend::TextSpan span,
                                                  const frontend::Expression &expr,
                                                  const frontend::Declaration &template_decl,
                                                  const bool named,
                                                  std::vector<TypeId> explicit_args) {
    const size_t field_count = template_decl.parameters.size();
    std::vector<TypeId> template_field_types;
    template_field_types.reserve(field_count);
    {
        const uint32_t saved_decl_id            = currentDeclId_;
        const frontend::FunctionKind saved_kind = currentFunctionKind_;
        currentDeclId_                          = template_decl.id.value;
        currentFunctionKind_                    = frontend::FunctionKind::Standard;
        for (const auto &param : template_decl.parameters) {
            const TypeId lowered = lowerTypeExpr(param.type);
            template_field_types.push_back(lowered ? lowered : error_type);
        }
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
    }

    std::vector<bool> seen(field_count, false);
    std::vector<size_t> provided_field_indices;
    std::vector<size_t> provided_operands;
    std::vector<TypeId> declared_field_types;
    std::vector<TypeId> argument_types;
    provided_field_indices.reserve(expr.operands.size());
    provided_operands.reserve(expr.operands.size());
    declared_field_types.reserve(expr.operands.size());
    argument_types.reserve(expr.operands.size());

    for (size_t i = 0; i < expr.operands.size(); ++i) {
        int decl_idx = -1;
        if (named) {
            const std::string_view wanted = i < expr.field_names.size()
                                                ? std::string_view(expr.field_names[i])
                                                : std::string_view{};
            for (size_t index = 0; index < template_decl.parameters.size(); ++index) {
                if (template_decl.parameters[index].name == wanted) {
                    decl_idx = static_cast<int>(index);
                    break;
                }
            }
            if (decl_idx < 0) {
                report(expr.span,
                       "unknown field '" + std::string(wanted) + "' in struct '" +
                           template_decl.name + "'",
                       diagnostics::err::NoMember);
                continue;
            }
        } else {
            if (i >= field_count) {
                report(expr.span,
                       "too many fields in struct literal for '" + template_decl.name + "'",
                       diagnostics::err::TypeMismatch);
                continue;
            }
            decl_idx = static_cast<int>(i);
        }

        if (seen[static_cast<size_t>(decl_idx)]) {
            report(expr.span,
                   "duplicate field '" +
                       template_decl.parameters[static_cast<size_t>(decl_idx)].name +
                       "' in struct literal",
                   diagnostics::err::TypeMismatch);
            continue;
        }
        seen[static_cast<size_t>(decl_idx)] = true;

        const auto &operand     = snapshot.expressions()[expr.operands[i].value - 1U];
        const auto *template_st = type_table.struct_type(type_table.stripQualifiers(
            type_table.lookupNamed(std::string_view(template_decl.name))));
        const bool visible_template_field =
            template_st != nullptr && fieldVisible(*template_st, static_cast<size_t>(decl_idx));
        if (operand.kind == frontend::ExprKind::Placeholder) {
            if (!findFieldDefault(template_decl.name, static_cast<size_t>(decl_idx))) {
                report(expr.span,
                       "field '" + template_decl.parameters[static_cast<size_t>(decl_idx)].name +
                           "' has no default value for '_'",
                       diagnostics::err::TypeMismatch);
            }
            continue;
        }
        if (!visible_template_field) {
            report(expr.span,
                   "field '" +
                       (named ? std::string(expr.field_names[i])
                              : std::string(
                                    template_decl.parameters[static_cast<size_t>(decl_idx)].name)) +
                       "' of struct '" + template_decl.name +
                       "' is private in a struct literal; use a public accessor",
                   diagnostics::err::NoMember);
            continue;
        }
        const TypeId value_type = inferExpr(expr.operands[i]);
        if (value_type == error_type)
            return error_type;
        provided_field_indices.push_back(static_cast<size_t>(decl_idx));
        provided_operands.push_back(i);
        declared_field_types.push_back(template_field_types[static_cast<size_t>(decl_idx)]);
        argument_types.push_back(value_type);
    }

    std::vector<TypeId> resolved_args;
    if (instantiations == nullptr) {
        report(span, "generic struct literals require the instantiation pass",
               diagnostics::err::GenericCannotInfer);
        return error_type;
    }
    const comptime::GenericResolveStatus status = instantiations->resolveStruct(
        template_decl.genericParams.size(), template_decl.id.value, explicit_args,
        declared_field_types, argument_types, resolved_args);
    switch (status) {
    case comptime::GenericResolveStatus::Arity:
        report(span, "wrong generic argument count for '" + template_decl.name + "'",
               diagnostics::err::GenericArity);
        return error_type;
    case comptime::GenericResolveStatus::CannotInfer:
        report(span,
               "cannot infer generic struct literal for '" + template_decl.name +
                   "'; field types do not uniquely determine all generic parameters",
               diagnostics::err::GenericStructInfer);
        return error_type;
    case comptime::GenericResolveStatus::Explosion:
        report(span, "too many generic instantiations", diagnostics::err::GenericExplosion);
        return error_type;
    case comptime::GenericResolveStatus::Ok:
        break;
    }

    const TypeId concrete = instantiateStructFromArgs(span, template_decl, resolved_args);
    if (!concrete)
        return error_type;
    const TypeId concrete_resolved = type_table.stripQualifiers(concrete);
    const auto *st                 = type_table.struct_type(concrete_resolved);
    if (st == nullptr) {
        report(span, "'" + template_decl.name + "' is not a struct type",
               diagnostics::err::GenericCannotInfer);
        return error_type;
    }

    for (size_t i = 0; i < provided_field_indices.size(); ++i) {
        const size_t field_index = provided_field_indices[i];
        const TypeId field_type  = st->fields[field_index];
        const TypeId value_type  = argument_types[i];
        if (!coerceValue(expr.operands[provided_operands[i]], field_type, value_type)) {
            reportCoercionFailure(
                expr.span, field_type, value_type,
                "struct literal field type mismatch for '" +
                    (named ? expr.field_names[provided_operands[i]]
                           : std::string(template_decl.parameters[field_index].name)) +
                    "'");
            return error_type;
        }
        if (pointerAliasEscapesScope(expr.operands[provided_operands[i]])) {
            report(expr.span, "pointer to local storage cannot escape the current scope",
                   diagnostics::err::PointerEscapesScope);
        }
    }

    for (size_t i = 0; i < field_count; ++i) {
        if (seen[i] || findFieldDefault(template_decl.name, i) || !fieldVisible(*st, i))
            continue;
        report(expr.span,
               "missing field '" + template_decl.parameters[i].name +
                   "' in struct literal; add a value or a field default",
               diagnostics::err::TypeMismatch);
    }
    return TypeId{concrete_resolved.intern_seq};
}
TypeId PerModuleSema::inferStructLiteral(frontend::ExprId id) {
    const auto &expr             = snapshot.expressions()[id.value - 1U];
    std::string struct_name      = expr.text;
    TypeId struct_tid            = kInvalidTypeId;
    TypeId resolved              = kInvalidTypeId;
    const StructType *st         = nullptr;
    bool from_generic_args       = false;
    const bool qualified_literal = struct_name.find('.') != std::string::npos;
    if (qualified_literal) {
        const auto *resolved_literal = findResolvedExpr(id);
        if (resolved_literal == nullptr ||
            resolved_literal->kind != session::ResolutionKind::Import) {
            report(expr.span,
                   "qualified struct literal '" + struct_name +
                       "' does not resolve to an imported type",
                   diagnostics::err::UndefinedIdent);
            return error_type;
        }
        struct_tid = typeOfResolvedName(id);
        if (!struct_tid || type_table.kindOf(resolve(struct_tid)) == TypeKind::Union) {
            const auto *union_data =
                struct_tid ? type_table.union_type(resolve(struct_tid)) : nullptr;
            if (union_data != nullptr)
                return inferUnionLiteral(id, struct_tid, *union_data);
            report(expr.span, "unknown struct type '" + struct_name + "'",
                   diagnostics::err::UndefinedIdent);
            return error_type;
        }
        resolved          = resolve(struct_tid);
        st                = type_table.struct_type(resolved);
        from_generic_args = false;
    }
    if (!expr.genericArgs.empty()) {
        from_generic_args         = true;
        const TypeId instantiated = instantiateTypeExpr(expr.span, expr.text, expr.genericArgs);
        if (!instantiated) {
            return error_type;
        }
        const TypeId instantiated_resolved = type_table.stripQualifiers(instantiated);
        if (const auto *union_data = type_table.union_type(instantiated_resolved)) {
            return inferUnionLiteral(id, instantiated, *union_data);
        }
        if (type_table.struct_type(instantiated_resolved) == nullptr) {
            report(expr.span, "'" + expr.text + "' is not a generic struct type",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        struct_tid = instantiated;
        resolved   = instantiated_resolved;
        st         = type_table.struct_type(resolved);
    }
    if (!from_generic_args && !qualified_literal) {
        for (const auto &decl : snapshot.declarations()) {
            if (decl.kind == frontend::DeclKind::Struct && decl.name == expr.text &&
                !decl.genericParams.empty()) {
                return resolveGenericStructLiteral(expr.span, expr, decl, !expr.field_names.empty(),
                                                   {});
            }
        }
        struct_tid = type_table.lookupNamed(struct_name);
        if (struct_tid && type_table.kindOf(resolve(struct_tid)) == TypeKind::Union) {
            const auto *union_data = type_table.union_type(resolve(struct_tid));
            if (union_data != nullptr)
                return inferUnionLiteral(id, struct_tid, *union_data);
        }
    }
    if (!from_generic_args && !qualified_literal) {
        struct_tid = type_table.lookupNamed(struct_name);
        if (!struct_tid) {
            report(expr.span, "unknown struct type '" + struct_name + "'",
                   diagnostics::err::UndefinedIdent);
            return error_type;
        }
        resolved = resolve(struct_tid);
        st       = type_table.struct_type(resolved);
    }
    if (st == nullptr) {
        report(expr.span, "'" + struct_name + "' is not a struct type");
        return error_type;
    }
    const size_t field_count = st->fields.size();
    const bool named         = !expr.field_names.empty();
    const auto fieldName     = [&](const int index) -> std::string {
        if (index >= 0 && static_cast<size_t>(index) < st->field_names.size())
            return std::string(st->field_names[static_cast<size_t>(index)]);
        return struct_name;
    };
    std::vector<bool> seen(field_count, false);
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        int decl_idx = -1;
        if (named) {
            decl_idx = type_table.fieldIndex(resolved, expr.field_names[i]);
            if (decl_idx < 0) {
                report(expr.span,
                       "unknown field '" + expr.field_names[i] + "' in struct '" + struct_name +
                           "'",
                       diagnostics::err::NoMember);
                continue;
            }
        } else {
            decl_idx = static_cast<int>(i);
            if (i >= field_count) {
                report(expr.span, "too many fields in struct literal for '" + struct_name + "'",
                       diagnostics::err::TypeMismatch);
                continue;
            }
        }
        if (seen[static_cast<size_t>(decl_idx)]) {
            report(expr.span, "duplicate field '" + fieldName(decl_idx) + "' in struct literal",
                   diagnostics::err::TypeMismatch);
            continue;
        }
        seen[static_cast<size_t>(decl_idx)] = true;
        if (!fieldVisible(*st, static_cast<size_t>(decl_idx))) {
            report(expr.span,
                   "field '" + fieldName(decl_idx) + "' of struct '" + struct_name +
                       "' is private in a struct literal; use a public accessor",
                   diagnostics::err::NoMember);
            continue;
        }
        const TypeId decl_type = st->fields[static_cast<size_t>(decl_idx)];
        const auto &operand    = snapshot.expressions()[expr.operands[i].value - 1U];
        if (operand.kind == frontend::ExprKind::Placeholder) {
            if (!findFieldDefault(expr.text, static_cast<size_t>(decl_idx))) {
                report(expr.span,
                       "field '" + fieldName(decl_idx) + "' has no default value for '_'",
                       diagnostics::err::TypeMismatch);
            }
            continue;
        }
        const TypeId value_type = inferExpr(expr.operands[i]);
        if (!coerceValue(expr.operands[i], decl_type, value_type)) {
            reportCoercionFailure(expr.span, decl_type, value_type,
                                  "struct literal field type mismatch for '" +
                                      (named ? expr.field_names[i] : fieldName(decl_idx)) + "'");
            return error_type;
        }
        if (pointerAliasEscapesScope(expr.operands[i])) {
            report(expr.span, "pointer to local storage cannot escape the current scope",
                   diagnostics::err::PointerEscapesScope);
        }
    }
    for (size_t i = 0; i < field_count; ++i) {
        if (seen[i] || findFieldDefault(expr.text, i))
            continue;
        report(expr.span,
               "missing field '" + fieldName(static_cast<int>(i)) +
                   "' in struct literal; add a value or a field default",
               diagnostics::err::TypeMismatch);
    }
    return TypeId{resolved.intern_seq};
}
TypeId PerModuleSema::inferPackLiteral(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    auto &members    = type_table.makeTypeStorage();
    auto &names      = type_table.makeStringStorage();
    for (const auto operand : expr.operands)
        members.push(inferExpr(operand));
    return type_table.internPack(members, names);
}
TypeId PerModuleSema::inferUnionLiteral(frontend::ExprId id, TypeId union_tid,
                                        const UnionType &union_data) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.field_names.size() != 0U) {
        report(expr.span, "positional raw union literals do not accept named members",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    if (expr.operands.size() != 1U) {
        report(expr.span, "raw union literal requires exactly one member value",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    TypeId chosen_member = kInvalidTypeId;
    for (const auto member : union_data.members) {
        const TypeId member_type = resolve(member);
        const TypeId value_type  = inferExpr(expr.operands[0]);
        if (member_type == error_type || value_type == error_type)
            return error_type;
        if (coerceValue(expr.operands[0], member_type, value_type)) {
            chosen_member = member_type;
            break;
        }
    }
    if (!chosen_member) {
        const TypeId value_type = inferExpr(expr.operands[0]);
        reportCoercionFailure(expr.span, union_data.members[0], value_type,
                              "raw union member type mismatch");
        return error_type;
    }
    return TypeId{union_tid.intern_seq};
}
TypeId PerModuleSema::inferArrayLiteral(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return type_table.internArray(i32_type, 0);

    TypeId elem = error_type;
    for (const auto operand : expr.operands) {
        TypeId t = inferExpr(operand);
        if (elem == error_type) {
            elem = t;
            continue;
        }
        if (!sameType(elem, t)) {
            if (adaptNumericLiteral(operand, elem))
                continue;
            // The annotated array literal can still carry a coercion such as
            // `[2]*char = [s, s]`, where each slice element becomes a pointer.
            // Only use the first element's type when it is already the pointer
            // storage expected by the surrounding binding; otherwise the array
            // literal remains heterogeneous and this path reports as before.
            if (type_table.kindOf(resolve(elem)) == TypeKind::Pointer &&
                coerceValue(operand, elem, t))
                continue;
            report(expr.span, "array literal element types do not match",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
    }
    // Escape checking runs on the coerced expression, so `[s, s]` stored as an
    // array of `*char` reports E4008 even though the literal's inferred element
    // type was `[]char`.
    for (const auto operand : expr.operands) {
        if (pointerAliasEscapesScope(operand)) {
            report(expr.span, "pointer to local storage cannot escape the current scope",
                   diagnostics::err::PointerEscapesScope);
        }
    }
    if (elem == error_type)
        return error_type;
    return type_table.internArray(elem, expr.operands.size());
}
frontend::ExprId PerModuleSema::findFieldDefault(std::string_view struct_name,
                                                 size_t field_index) const noexcept {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Struct || decl.name != struct_name)
            continue;
        if (field_index < decl.parameters.size())
            return decl.parameters[field_index].defaultValue;
        break;
    }
    return {};
}

} // namespace zith::sema::modern
