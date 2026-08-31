#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <algorithm>
#include <string>

namespace zith::sema::modern {

TypeId PerModuleSema::pointerBase(TypeId type) const noexcept {
    const TypeId resolved = resolve(type);
    TypeId base           = kInvalidTypeId;
    if (type_table.kindOf(resolved) == TypeKind::Pointer) {
        base = resolved;
    } else if (type_table.kindOf(resolved) == TypeKind::Optional) {
        // Exactly one level of `Optional` is unwrapped, because every imported C pointer is
        // `?*T`; `??*T` is deliberately not treated as a pointer.
        if (const auto *opt = type_table.optional(resolved)) {
            const TypeId inner = resolve(opt->inner);
            if (type_table.kindOf(inner) == TypeKind::Pointer)
                base = inner;
        }
    }
    return base;
}
bool PerModuleSema::isNullablePointer(TypeId type) const noexcept {
    const TypeId resolved = resolve(type);
    return type_table.kindOf(resolved) == TypeKind::Optional &&
           static_cast<bool>(pointerBase(resolved));
}
bool PerModuleSema::isVoidPointer(TypeId type) const noexcept {
    const TypeId ptr = pointerBase(type);
    if (!ptr)
        return false;
    const auto *info = type_table.pointer(ptr);
    return info != nullptr && info->pointee &&
           type_table.canonical(info->pointee) == type_table.canonical(void_type);
}

/// True for `raw opaque as *T` and `*T as raw opaque`: both sides are pointers (each
/// possibly wrapped in one `Optional`, as every C pointer is) and at least one points to
/// `void`.
bool PerModuleSema::isOpaquePointerCast(TypeId from, TypeId to) const {
    const TypeId from_ptr = pointerBase(from);
    const TypeId to_ptr   = pointerBase(to);
    if (!from_ptr || !to_ptr)
        return false;
    return isVoidPointer(from_ptr) || isVoidPointer(to_ptr);
}
TypeId PerModuleSema::unionMemberType(frontend::TextSpan span, TypeId union_type, TypeId member) {
    const TypeId resolved  = resolve(union_type);
    const auto *union_data = type_table.union_type(resolved);
    if (union_data == nullptr)
        return error_type;
    const TypeId member_resolved = resolve(member);
    for (const auto candidate : union_data->members) {
        if (sameType(resolve(candidate), member_resolved))
            return member;
    }
    report(span,
           "'" + type_table.typeToString(member) + "' is not a member of '" +
               type_table.typeToString(resolved) + "'",
           diagnostics::err::InvalidCast);
    return error_type;
}
TypeId PerModuleSema::inferCast(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    // `as` casts through an unregistered type name are a barrier: report a single
    // UnsupportedSyntax instead of letting the unknown type cascade into 2001+3003.
    const auto &target_type = snapshot.typeExpressions()[expr.cast_type.value - 1U];
    if (target_type.kind == frontend::TypeExprKind::Name &&
        !type_table.lookupNamed(target_type.name) &&
        !isGenericTypeParamName(target_type.name, currentDeclId_)) {
        report(expr.span, "'as' casts to unknown types are not supported in this version",
               diagnostics::err::UnsupportedSyntax);
        return error_type;
    }
    TypeId source = inferExpr(expr.operands[0]);
    if (type_table.kindOf(resolve(source)) == TypeKind::Pointer &&
        (isSelfReceiver(expr.operands[0]) || isBorrowParameter(expr.operands[0]))) {
        if (const auto *ptr = type_table.pointer(resolve(source)))
            source = ptr->pointee;
    }
    const TypeId target = lowerTypeExpr(expr.cast_type);
    TypeId result       = target;
    if (!target) {
        report(expr.span, "unknown target type in 'as' conversion", diagnostics::err::TypeMismatch);
        result = error_type;
    } else if (source && source != error_type) {
        // `T as Nominal`/`Nominal as T` wraps or unwraps the nominal's single
        // underlying field. This is the explicit construction/extraction path
        // for `type Name = T` until a dedicated struct-literal syntax lands.
        const auto *source_nominal = type_table.nominal(resolve(source));
        const auto *target_nominal = type_table.nominal(resolve(target));
        const TypeId nominal_target =
            source_nominal ? source_nominal->target
                           : (target_nominal ? target_nominal->target : kInvalidTypeId);
        const TypeId other = source_nominal ? resolve(target) : resolve(source);
        if (nominal_target && type_table.kindOf(other) == type_table.kindOf(nominal_target) &&
            sameType(nominal_target, other)) {
            return result;
        }
        const TypeId from_resolved = resolve(source);
        const TypeId to_resolved   = resolve(target);
        // Bare `opaque` is tagged: any concrete value erases into it, and
        // checked extraction returns `?T` so the tag-match/none path is visible.
        if (type_table.kindOf(to_resolved) == TypeKind::Opaque) {
            if (type_table.kindOf(from_resolved) == TypeKind::Opaque)
                return result;
            return opaque_type;
        }
        if (type_table.kindOf(from_resolved) == TypeKind::Opaque) {
            if (type_table.kindOf(to_resolved) == TypeKind::Opaque)
                return result;
            // `opaque as raw opaque` reinterprets the opaque payload pointer as
            // `void*`. It is deliberate and unchecked, so it must not go down the
            // optional-checked path.
            if (type_table.kindOf(to_resolved) == TypeKind::Pointer &&
                type_table.pointer(to_resolved) != nullptr &&
                type_table.canonical(type_table.pointer(to_resolved)->pointee) ==
                    type_table.canonical(void_type)) {
                return result;
            }
            return expr.is_raw ? to_resolved : type_table.internOptional(to_resolved);
        }
        if (type_table.kindOf(from_resolved) == TypeKind::Union) {
            const auto *union_data = type_table.union_type(from_resolved);
            const bool is_tagged   = union_data != nullptr && union_data->is_tagged;
            if (is_tagged && !expr.is_raw) {
                report(expr.span,
                       "member access on a tagged union requires a checked/narrowed context; "
                       "use 'raw f as Member' to bypass the tag check",
                       diagnostics::err::InvalidCast);
                return error_type;
            }
            bool has_template_param = false;
            if (union_data != nullptr) {
                for (const auto member : union_data->members) {
                    if (type_table.kindOf(resolve(member)) == TypeKind::GenericParam)
                        has_template_param = true;
                    if (sameType(resolve(member), resolve(to_resolved)))
                        return result;
                }
            }
            // Methods declared inside `union Any<T, U>` are checked against the
            // template, where members are opaque GenericParam types. Concrete
            // receivers are validated after monomorphization, so accept the
            // target here and let HIR/codegen use the concrete member index.
            if (has_template_param)
                return result;
            return unionMemberType(expr.span, from_resolved, to_resolved);
        }
        if (type_table.kindOf(to_resolved) == TypeKind::Union) {
            const auto *union_type = type_table.union_type(to_resolved);
            if (union_type == nullptr)
                return error_type;
            for (const auto member : union_type->members) {
                if (sameType(resolve(member), from_resolved))
                    return result;
            }
            report(expr.span,
                   "'" + type_table.typeToString(from_resolved) + "' is not a member of union '" +
                       type_table.typeToString(to_resolved) + "'",
                   diagnostics::err::InvalidCast);
            return error_type;
        }
        CastKind kind =
            classifyCast(type_table.kindOf(from_resolved), type_table.kindOf(to_resolved));
        // `raw opaque as *T` and `*T as raw opaque` are the two supported pointer casts.
        // Pointer-to-pointer between two concrete pointee types stays invalid, as does any
        // integer/pointer mix, so `as` never silently reinterprets an address.
        if (kind == CastKind::Invalid && isOpaquePointerCast(from_resolved, to_resolved))
            kind = CastKind::PtrToPtr;
        // Dropping nullability silently would defeat `?*T`: a C pointer must be rewritten
        // as `as ?*T`, keeping the null case visible in the type.
        if (kind == CastKind::PtrToPtr && isNullablePointer(from_resolved) &&
            !isNullablePointer(to_resolved)) {
            report(expr.span,
                   "cannot cast a nullable C pointer to a non-nullable pointer; use 'as ?*T'",
                   diagnostics::err::InvalidCast);
            result = error_type;
        } else if (kind == CastKind::Invalid) {
            report(expr.span,
                   "'as' supports numeric conversions and 'raw opaque' pointer conversions",
                   diagnostics::err::InvalidCast);
            result = error_type;
        }
    }
    return result;
}
TypeId PerModuleSema::inferIsNull(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId operand = inferExpr(expr.operands[0]);
    if (type_table.kindOf(resolve(operand)) == TypeKind::Pointer &&
        (isSelfReceiver(expr.operands[0]) || isBorrowParameter(expr.operands[0]))) {
        if (const auto *ptr = type_table.pointer(resolve(operand)))
            operand = ptr->pointee;
    }
    if (operand == error_type || !operand)
        return error_type;
    if (type_table.kindOf(resolve(operand)) != TypeKind::Optional) {
        report(expr.span, "'is null' requires an optional operand ('?T')",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    return bool_type;
}
TypeId PerModuleSema::inferIsType(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty() || !expr.cast_type)
        return error_type;
    TypeId operand = inferExpr(expr.operands[0]);
    if (type_table.kindOf(resolve(operand)) == TypeKind::Pointer &&
        (isSelfReceiver(expr.operands[0]) || isBorrowParameter(expr.operands[0]))) {
        if (const auto *ptr = type_table.pointer(resolve(operand)))
            operand = ptr->pointee;
    }
    if (operand == error_type || !operand)
        return error_type;
    const TypeId operand_resolved = resolve(operand);
    if (type_table.kindOf(operand_resolved) == TypeKind::Opaque) {
        const TypeId target = lowerTypeExpr(expr.cast_type);
        if (!target) {
            report(expr.span, "unknown target type in 'is' test", diagnostics::err::TypeMismatch);
            return error_type;
        }
        return bool_type;
    }
    const auto *union_data = type_table.union_type(operand_resolved);
    if (union_data == nullptr || !union_data->is_tagged) {
        report(expr.span, "'is Type' requires an operand whose type is a tagged union",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    const TypeId target_template = lowerTypeExpr(expr.cast_type);
    if (!target_template) {
        report(expr.span, "unknown target type in 'is' test", diagnostics::err::TypeMismatch);
        return error_type;
    }
    const TypeId target =
        instantiations != nullptr
            ? instantiations->substituteType(target_template, unionArgsFor(operand_resolved))
            : target_template;
    const auto *union_concrete = type_table.union_type(operand_resolved);
    if (union_concrete != nullptr) {
        bool has_template_param = false;
        for (const auto member : union_concrete->members) {
            if (type_table.kindOf(resolve(member)) == TypeKind::GenericParam)
                has_template_param = true;
            if (sameType(resolve(member), resolve(target)))
                return bool_type;
        }
        // The body of an owner method is checked against the generic template,
        // where members are opaque GenericParam types (`Any<T, U>`). Concrete
        // receiver validation happens after monomorphization; accept a target
        // here and let HIR/codegen use the concrete member index.
        if (has_template_param)
            return bool_type;
    }
    report(expr.span,
           "'" + type_table.typeToString(target) + "' is not a member of tagged union '" +
               type_table.typeToString(operand_resolved) + "'",
           diagnostics::err::InvalidCast);
    return error_type;
}
TypeId PerModuleSema::inferRange(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() != 2U)
        return error_type;
    const TypeId lo = inferExpr(expr.operands[0]);
    const TypeId hi = inferExpr(expr.operands[1]);
    if (lo == error_type || hi == error_type)
        return error_type;
    if (!sameType(lo, hi)) {
        if (!adaptNumericLiteral(expr.operands[1], lo)) {
            report(expr.span, "range pattern bounds must have the same type",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
    }
    return bool_type;
}
TypeId PerModuleSema::inferWhen(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    const TypeId subject = inferExpr(expr.operands[0]);
    if (subject == error_type)
        return error_type;
    const size_t case_count = expr.operands.size() - 1U;
    bool has_default        = false;
    TypeId body_type        = void_type;
    for (size_t i = 0; i < case_count; ++i) {
        const frontend::ExprId condition =
            i < expr.conditions.size() ? expr.conditions[i] : frontend::ExprId{};
        const bool is_default = !condition;
        if (is_default) {
            has_default = true;
            if (i + 1U != case_count) {
                report(expr.span, "a default when case ('_') must be the last case",
                       diagnostics::err::TypeMismatch);
            }
            continue;
        }
        const auto &cond_node  = snapshot.expressions()[condition.value - 1U];
        const TypeId cond_type = inferExpr(condition);
        if (cond_node.kind == frontend::ExprKind::Range) {
            // Range pattern `lo..hi`: the subject must be comparable with the bounds.
            const auto &lo_node = snapshot.expressions()[cond_node.operands[0].value - 1U];
            const TypeId bound  = inferExpr(cond_node.operands[0]);
            if (!sameType(subject, bound)) {
                report(lo_node.span, "when range pattern must match the subject type",
                       diagnostics::err::TypeMismatch);
            }
        } else if (cond_type != bool_type && cond_type != error_type &&
                   type_table.optional(resolve(cond_type)) == nullptr) {
            // A non-boolean condition is an equality pattern: `(0)` means `subject == 0`.
            if (!sameType(subject, cond_type) && !adaptNumericLiteral(condition, subject)) {
                report(expr.span,
                       "when case condition must be a boolean expression or match the subject "
                       "type",
                       diagnostics::err::TypeMismatch);
            }
        }
        // An `(f is Member)` case narrows `f` to the member type for the body,
        // matching the existing `if` flow-typing rule.
        frontend::LocalId narrowed_local;
        TypeId original_local_type = kInvalidTypeId;
        TypeId narrowed_type       = kInvalidTypeId;
        if (cond_node.kind == frontend::ExprKind::IsType && !cond_node.operands.empty() &&
            cond_node.cast_type) {
            if (const auto *resolved = findResolvedExpr(cond_node.operands[0]);
                resolved != nullptr && resolved->local) {
                narrowed_local      = resolved->local;
                original_local_type = typeOfLocal(narrowed_local);
                narrowed_type       = lowerTypeExpr(cond_node.cast_type);
                const TypeKind local_kind =
                    type_table.kindOf(resolve(type_table.stripQualifiers(original_local_type)));
                if (narrowed_type &&
                    (local_kind == TypeKind::Opaque || local_kind == TypeKind::Union))
                    setLocalType(narrowed_local, narrowed_type);
            }
        }
        const TypeId case_type = inferExpr(expr.operands[i + 1U]);
        if (narrowed_local && narrowed_type)
            setLocalType(narrowed_local, original_local_type);
        if (i == 0U) {
            body_type = case_type;
        } else if (!sameType(body_type, case_type) && case_type != error_type) {
            report(expr.span, "when case bodies must all have the same type",
                   diagnostics::err::TypeMismatch);
        }
    }
    // A value-producing when needs a default case; without one the subject may not be
    // exhausted (the legacy result was an optional, which the modern pipeline does not
    // synthesize for when).
    if (body_type != void_type && !has_default) {
        report(expr.span, "non-exhaustive when; add a default case '(_) ~> ...'",
               diagnostics::err::TypeMismatch);
    }
    return body_type;
}
TypeId PerModuleSema::inferLayoutIntrinsic(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.text == "lengthOf" || expr.text == "ptrOf") {
        if (expr.operands.empty()) {
            report(expr.span, "'@" + expr.text + "' requires a value argument",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        const TypeId operand     = inferExpr(expr.operands[0]);
        const TypeId resolved    = resolve(operand);
        const auto *slice        = type_table.slice(resolved);
        const auto *array        = type_table.array(resolved);
        const auto &operand_expr = snapshot.expressions()[expr.operands[0].value - 1U];
        const bool is_string_literal =
            operand_expr.kind == frontend::ExprKind::Literal && looksString(operand_expr.text);
        const TypeId string_literal_ty =
            is_string_literal ? type_table.internPointer(char_type) : kInvalidTypeId;
        bool accepts_value =
            slice != nullptr || array != nullptr || (is_string_literal && string_literal_ty);
        if (!accepts_value && type_table.kindOf(resolved) == TypeKind::Pointer) {
            // `@ptrOf` on a pointer is identity-like; it keeps accepting the
            // pointer objects already exposed by C interop.
            accepts_value = expr.text == "ptrOf";
        }
        if (!accepts_value) {
            report(expr.span, "'@" + expr.text + "' requires a slice, array, or string literal",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        if (expr.text == "ptrOf") {
            const bool tied_to_local = operand_expr.kind != frontend::ExprKind::Literal ||
                                       (slice != nullptr || array != nullptr);
            if (tied_to_local)
                escapingPointerExprs_.insert(id.value);
            if (is_string_literal)
                return type_table.internPointer(char_type);
            if (slice != nullptr)
                return type_table.internPointer(slice->element);
            if (array != nullptr)
                return type_table.internPointer(array->element);
            return resolved;
        }
        return type_table.lookupNamed("u64");
    }
    const TypeId target = lowerTypeExpr(expr.cast_type);
    if (!target)
        return error_type;
    const TypeId resolved = resolve(target);
    // @sizeOf applies to any complete type (primitives and structs alike) and
    // reports its size in bytes as u64; offsetOf/alignOf stay struct-only.
    if (expr.text == "sizeOf") {
        if (type_table.kindOf(resolved) == TypeKind::Void) {
            report(expr.span, "'@sizeOf' requires a complete type", diagnostics::err::TypeMismatch);
            return error_type;
        }
        return type_table.lookupNamed("u64");
    }
    if (type_table.kindOf(resolved) != TypeKind::Struct) {
        report(expr.span, "'@" + expr.text + "' requires a struct type",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    if (expr.text == "offsetOf") {
        if (expr.field_names.empty()) {
            report(expr.span, "'@offsetOf' requires a field name", diagnostics::err::TypeMismatch);
            return error_type;
        }
        if (type_table.fieldIndex(resolved, expr.field_names[0]) < 0) {
            report(expr.span, "unknown field '" + expr.field_names[0] + "'",
                   diagnostics::err::NoMember);
            return error_type;
        }
    }
    return i32_type;
}
bool PerModuleSema::adaptNumericLiteral(frontend::ExprId value, TypeId target) {
    if (!value || value.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[value.value - 1U];
    // `-1` parses as a unary minus over a literal; adapt through it.
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "-" && !expr.operands.empty()) {
        if (!adaptNumericLiteral(expr.operands[0], target))
            return false;
        setExprType(value, target);
        return true;
    }
    if (expr.kind != frontend::ExprKind::Literal)
        return false;
    const TypeKind target_kind = type_table.kindOf(resolve(target));
    const bool integer_literal = looksInteger(expr.text);
    const bool float_literal   = looksFloat(expr.text);
    if (!integer_literal && !float_literal)
        return false;
    if (target_kind == TypeKind::Integer && !integer_literal)
        return false;
    if (target_kind != TypeKind::Integer && target_kind != TypeKind::Float)
        return false;
    if (integer_literal) {
        const auto suffix = support::integerSuffix(expr.text);
        if (!suffix.empty()) {
            const TypeId suffix_type = type_table.lookupNamed(suffix);
            if (!suffix_type || !sameType(resolve(target), suffix_type))
                return false;
        }
    }
    setExprType(value, target);
    return true;
}
bool PerModuleSema::coerceValue(frontend::ExprId value, TypeId target, TypeId source) {
    // Any concrete value can be erased into bare `opaque`. The original source
    // type is recorded because HIR/codegen must know which concrete tag and
    // payload layout the opaque value stores.
    if (value && source && type_table.kindOf(resolve(target)) == TypeKind::Opaque &&
        type_table.kindOf(resolve(source)) != TypeKind::Opaque) {
        setExprType(value, target);
        typed_map.opaqueSourceTypes.insert(value.value, source);
        return true;
    }
    // A borrowed parameter's ABI is a pointer, but the call-site argument is
    // the borrowed value. `lend q` / `view q` check the value against the
    // pointee after the ownership-annotation check has run.
    if (isBorrowParamType(target)) {
        TypeId cursor = target;
        for (unsigned guard = 0; guard < 8U; ++guard) {
            cursor = type_table.canonical(cursor);
            if (const auto *qualified = type_table.qualified(cursor); qualified != nullptr) {
                cursor = qualified->inner;
                continue;
            }
            break;
        }
        if (const auto *pointer = type_table.pointer(cursor); pointer != nullptr)
            target = type_table.stripQualifiers(pointer->pointee);
    }
    // An annotated array literal may coerce per element. The array itself is
    // retyped to the annotation only when every operand accepts the target
    // element type, so `[2]*char = [s, s]` records each `[]char -> *char`
    // escape and HIR lowers each element to a pointer.
    if (value && value.value <= snapshot.expressions().size() && source &&
        !sameType(target, source)) {
        const auto &literal_expr = snapshot.expressions()[value.value - 1U];
        const auto *target_array = type_table.array(resolve(target));
        const auto *source_array = type_table.array(resolve(source));
        if (literal_expr.kind == frontend::ExprKind::ArrayLiteral && target_array != nullptr &&
            source_array != nullptr && source_array->size == target_array->size &&
            target_array->size == literal_expr.operands.size()) {
            bool all_elements = true;
            for (const frontend::ExprId element : literal_expr.operands) {
                const TypeId element_type = typeOfExpr(element);
                if (!element_type || element_type == error_type ||
                    !coerceValue(element, target_array->element, element_type)) {
                    all_elements = false;
                    break;
                }
            }
            if (all_elements) {
                setExprType(value, target);
                return true;
            }
        }
    }
    if (coercesTo(target, source)) {
        primeDynImplementations(target, source);
        // Record the optional target on a `null` literal so lowering can emit None directly.
        if (resolve(source) == null_type &&
            type_table.kindOf(resolve(target)) == TypeKind::Optional) {
            setExprType(value, target);
        } else if (value && value.value <= snapshot.expressions().size() &&
                   snapshot.expressions()[value.value - 1U].kind ==
                       frontend::ExprKind::PackLiteral &&
                   type_table.kindOf(resolve(target)) == TypeKind::Pack) {
            // A pack literal gets the annotated pack type so HIR can inherit
            // the declared member names for later field access.
            setExprType(value, target);
        }
        // `[]char -> *char` has the same lifetime implications as
        // `@ptrOf(slice)`: the pointer aliases the slice's backing storage.
        if (value && source && isCharSliceToPointer(source, target))
            markSlicePtrCoercionEscaping(value);
        return true;
    }
    // A real `state` declaration is assignable to the `state(params): ret`
    // value type only when the signature matches. The source type is the
    // declaration's normal `fn` shape; the target keeps Kind::State so the
    // dock call can preserve state call semantics.
    if (value && source && type_table.kindOf(resolve(target)) == TypeKind::State &&
        value.value <= snapshot.expressions().size()) {
        const auto &expr = snapshot.expressions()[value.value - 1U];
        const auto *resolved =
            expr.kind == frontend::ExprKind::Name ? findResolvedExpr(value) : nullptr;
        const frontend::Declaration *state_decl =
            resolved != nullptr ? declarationForResolved(*resolved) : nullptr;
        if (state_decl != nullptr && state_decl->kind == frontend::DeclKind::Function &&
            state_decl->functionKind == frontend::FunctionKind::State && sameType(target, source)) {
            setExprType(value, target);
            return true;
        }
    }
    if (type_table.kindOf(resolve(target)) == TypeKind::State &&
        (source == kInvalidTypeId || type_table.kindOf(resolve(source)) == TypeKind::Function))
        return false;
    // `lend q` / `view q` has the inner expression's type for overload/target
    // checks, but lowering turns the annotated node into an address. If the
    // value itself already is a borrow pointer, it can still be passed without
    // changing the outer ownership wrapper.
    if (value && value.value <= snapshot.expressions().size()) {
        const auto &arg = snapshot.expressions()[value.value - 1U];
        if (arg.kind == frontend::ExprKind::OwnershipCoerce && !arg.operands.empty() &&
            isBorrowParamType(target) && isBorrowParamType(source))
            return true;
    }
    // A string literal has a compile-time decoding length, so it can be
    // adapted to a `[]char` target without a runtime length. Sema types the
    // literal as the slice so lowering can emit `HirMakeSlice` around the
    // underlying `*char` payload.
    if (value && value.value <= snapshot.expressions().size()) {
        const auto &expr          = snapshot.expressions()[value.value - 1U];
        const auto *target_slice  = type_table.slice(type_table.stripQualifiers(target));
        const TypeId source_canon = type_table.stripQualifiers(source);
        if (expr.kind == frontend::ExprKind::Literal && looksString(expr.text) &&
            type_table.kindOf(source_canon) == TypeKind::Pointer && target_slice != nullptr &&
            sameType(type_table.stripQualifiers(target_slice->element), char_type)) {
            const auto *ptr = type_table.pointer(source_canon);
            if (ptr != nullptr && sameType(type_table.stripQualifiers(ptr->pointee), char_type)) {
                setExprType(value, target);
                return true;
            }
        }
    }
    return adaptNumericLiteral(value, target);
}
void PerModuleSema::primeDynImplementations(TypeId target, TypeId source) {
    if (instantiations == nullptr)
        return;
    const TypeId resolved_target = resolve(target);
    if (type_table.kindOf(resolved_target) != TypeKind::Dyn)
        return;
    const auto *dyn = type_table.dyn_type(resolved_target);
    if (dyn == nullptr)
        return;
    const TypeId source_resolved = resolve(source);
    const auto *enum_data        = type_table.enum_type(source_resolved);
    const auto *union_data       = type_table.union_type(source_resolved);
    if (enum_data == nullptr && union_data == nullptr)
        return;

    std::vector<TypeId> concrete_args;
    const frontend::DeclKind owner_kind =
        enum_data != nullptr ? frontend::DeclKind::Enum : frontend::DeclKind::Union;
    const std::string_view source_name = enum_data != nullptr ? enum_data->name : union_data->name;
    const std::string_view base_name   = source_name.find('<') != std::string_view::npos
                                             ? source_name.substr(0, source_name.find('<'))
                                             : source_name;
    const frontend::Declaration *template_decl = nullptr;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == owner_kind && decl.name == base_name && !decl.genericParams.empty()) {
            template_decl = &decl;
            break;
        }
    }
    if (template_decl == nullptr)
        return;
    if (union_data != nullptr) {
        concrete_args.assign(union_data->members.begin(), union_data->members.end());
    } else {
        const size_t degree = template_decl->genericParams.size();
        // Generic enums have no payload fields; recover the instance arguments
        // from the concrete name (`Status<i32>`). Primitive spellings are
        // resolved through the same registry as the type table.
        const std::string_view name = source_name;
        const char *open            = name.data();
        const char *end             = open + name.size();
        const char *lt              = std::find(open, end, '<');
        const char *close           = std::find(lt + 1, end, '>');
        const char *cursor          = lt + 1;
        while (cursor < close) {
            const char *comma = std::find(cursor, close, ',');
            const std::string_view text(cursor, static_cast<size_t>(comma - cursor));
            const size_t first = text.find_first_not_of(" \t");
            const size_t last  = text.find_last_not_of(" \t");
            const std::string_view trimmed =
                first != std::string_view::npos
                    ? text.substr(first, (last == std::string_view::npos ? text.size() : last) -
                                             first + 1U)
                    : std::string_view{};
            TypeId arg = type_table.lookupNamed(trimmed);
            if (arg == kInvalidTypeId) {
                if (trimmed == "i32")
                    arg = i32_type;
                else if (trimmed == "bool")
                    arg = bool_type;
                else if (trimmed == "char")
                    arg = char_type;
                else if (trimmed == "i64")
                    arg = i64_type;
                else if (trimmed == "f32")
                    arg = f32_type;
                else if (trimmed == "f64")
                    arg = f64_type;
            }
            if (arg)
                concrete_args.push_back(arg);
            if (comma == close)
                break;
            cursor = comma + 1;
        }
        while (concrete_args.size() < degree)
            concrete_args.push_back(kInvalidTypeId);
    }
    if (concrete_args.empty())
        return;

    const TypeId trait_target = resolve(dyn->target);
    const auto *trait_data    = type_table.trait(trait_target);
    if (trait_data == nullptr)
        return;
    const auto baseOf = [](std::string_view name) {
        if (const size_t angle = name.find('<'); angle != std::string_view::npos)
            return name.substr(0, angle);
        return name;
    };
    for (const auto &candidate : snapshot.declarations()) {
        if (candidate.kind != frontend::DeclKind::Function || candidate.ownerName.empty() ||
            candidate.traitName != trait_data->name || baseOf(candidate.ownerName) != base_name)
            continue;
        if (type_table.function(typeOfDecl(candidate.id)) == nullptr)
            continue;
        if (instantiations->bindCall(module, frontend::ExprId{}, module, candidate.id,
                                     concrete_args) == ~size_t{0})
            report(candidate.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
    }
}
void PerModuleSema::reportCoercionFailure(frontend::TextSpan span, TypeId target, TypeId source,
                                          std::string_view context, uint32_t fallback_code) {
    if (resolve(source) == null_type) {
        report(span, "cannot assign 'null' to a non-optional pointer; use '?*T'",
               diagnostics::err::TypeMismatch);
        return;
    }
    const TypeKind from = type_table.kindOf(resolve(source));
    const TypeKind to   = type_table.kindOf(resolve(target));
    if (from == TypeKind::Opaque || to == TypeKind::Opaque) {
        report(span,
               "implicit 'opaque' conversion is not allowed; use 'as' (or 'raw as' for an "
               "unchecked extraction)",
               diagnostics::err::TypeMismatch);
        return;
    }
    if (classifyCast(from, to) != CastKind::Invalid) {
        report(span, "implicit numeric conversion is not allowed; use 'as'",
               diagnostics::err::TypeMismatch);
        return;
    }
    const std::string target_str = type_table.typeToString(target);
    const std::string source_str = type_table.typeToString(source);
    if (!target_str.empty() && !source_str.empty()) {
        report(span,
               std::string(context) + ": expected '" + target_str + "', has type '" + source_str +
                   "'",
               fallback_code);
        return;
    }
    report(span, std::string(context), fallback_code);
}

} // namespace zith::sema::modern
