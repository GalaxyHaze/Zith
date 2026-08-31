#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <string>

namespace zith::sema::modern {

TypeId PerModuleSema::inferOptionalProp(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId operand = inferExpr(expr.operands[0]);
    if (operand == error_type || !operand)
        return error_type;
    TypeId resolved = resolve(operand);
    if (type_table.kindOf(resolved) != TypeKind::Optional) {
        report(expr.span, "'?' operator requires an optional operand",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    const auto *opt = type_table.optional(resolved);
    if (!opt)
        return error_type;
    // Verify enclosing function returns an optional that can accept this inner type
    if (currentReturnType_) {
        TypeId ret_resolved = resolve(currentReturnType_);
        if (type_table.kindOf(ret_resolved) != TypeKind::Optional) {
            report(expr.span, "'?' operator used in a function that does not return an optional",
                   diagnostics::err::TypeMismatch);
        }
    }
    return opt->inner;
}
TypeId PerModuleSema::inferIndex(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId result    = error_type;
    if (expr.operands.size() >= 2U) {
        const TypeId object = inferExpr(expr.operands[0]);
        const TypeId index  = inferExpr(expr.operands[1]);
        if (type_table.kindOf(resolve(index)) != TypeKind::Integer)
            report(expr.span, "array index must be an integer", diagnostics::err::TypeMismatch);
        const TypeId resolved_object = resolve(object);
        bool checked_container       = false;
        switch (type_table.kindOf(resolved_object)) {
        case TypeKind::Slice:
            if (const auto *slice = type_table.slice(resolved_object)) {
                result            = slice->element;
                checked_container = true;
            }
            break;
        case TypeKind::Array:
            if (const auto *array = type_table.array(resolved_object)) {
                result            = array->element;
                checked_container = true;
                if (!expr.is_raw) {
                    int64_t index_value = 0;
                    if (constantIntegerValue(expr.operands[1], index_value) &&
                        (index_value < 0 || static_cast<uint64_t>(index_value) >= array->size)) {
                        report(expr.span, "array index is out of bounds",
                               diagnostics::err::TypeMismatch);
                        return error_type;
                    }
                }
            }
            break;
        case TypeKind::Pointer:
            if (const auto *pointer = type_table.pointer(resolved_object))
                result = pointer->pointee;
            break;
        case TypeKind::Pack: {
            if (const auto *pack = type_table.pack(resolved_object)) {
                int64_t index_value = 0;
                if (!constantIntegerValue(expr.operands[1], index_value) || index_value < 0 ||
                    static_cast<uint64_t>(index_value) >= pack->members.size()) {
                    report(expr.span, "pack index is out of bounds or not a constant",
                           diagnostics::err::TypeMismatch);
                    return error_type;
                }
                result = pack->members[static_cast<size_t>(index_value)];
            }
            break;
        }
        case TypeKind::Error:
            break;
        default:
            report(expr.span, "type is not indexable", diagnostics::err::TypeMismatch);
            break;
        }
        if (result && !expr.is_raw && checked_container)
            result = type_table.internOptional(result);
    }
    return result;
}
void PerModuleSema::prepareLValueIndexTypes(frontend::ExprId id) {
    if (!id || id.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind != frontend::ExprKind::Index && expr.kind != frontend::ExprKind::Field &&
        expr.kind != frontend::ExprKind::Arrow)
        return;
    if (expr.kind != frontend::ExprKind::Index) {
        if (!expr.operands.empty())
            prepareLValueIndexTypes(expr.operands[0]);
        return;
    }
    if (expr.operands.empty())
        return;
    prepareLValueIndexTypes(expr.operands[0]);
    const TypeId object   = inferExpr(expr.operands[0]);
    const TypeId resolved = resolve(object);
    TypeId element        = kInvalidTypeId;
    if (const auto *array = type_table.array(resolved))
        element = array->element;
    else if (const auto *slice = type_table.slice(resolved))
        element = slice->element;
    else if (const auto *pointer = type_table.pointer(resolved))
        element = pointer->pointee;
    if (element) {
        const auto *array = type_table.array(resolved);
        if (array != nullptr && !expr.is_raw) {
            int64_t index_value = 0;
            if (constantIntegerValue(expr.operands[1], index_value) &&
                (index_value < 0 || static_cast<uint64_t>(index_value) >= array->size)) {
                report(expr.span, "array index is out of bounds", diagnostics::err::TypeMismatch);
                return;
            }
        }
        setExprType(id, element);
    }
}
bool PerModuleSema::constantIntegerValue(frontend::ExprId id, std::int64_t &out) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "-" && !expr.operands.empty()) {
        std::int64_t magnitude = 0;
        if (!constantIntegerValue(expr.operands[0], magnitude))
            return false;
        out = -magnitude;
        return true;
    }
    if (expr.kind != frontend::ExprKind::Literal || !looksInteger(expr.text))
        return false;
    return support::parseIntegerLiteral(expr.text, out) == support::IntLiteralStatus::Ok;
}
TypeId PerModuleSema::inferSliceRange(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId result    = error_type;
    if (expr.operands.size() < 3U)
        return result;

    const TypeId object = inferExpr(expr.operands[0]);
    const TypeId lower  = inferExpr(expr.operands[1]);
    const TypeId upper  = inferExpr(expr.operands[2]);
    if (type_table.kindOf(resolve(lower)) != TypeKind::Integer ||
        type_table.kindOf(resolve(upper)) != TypeKind::Integer) {
        report(expr.span, "slice bounds must be integers", diagnostics::err::TypeMismatch);
        return error_type;
    }
    if (type_table.kindOf(resolve(lower)) == TypeKind::Integer &&
        type_table.kindOf(resolve(upper)) == TypeKind::Integer &&
        !sameType(resolve(lower), resolve(upper))) {
        report(expr.span, "slice bounds must have the same integer type",
               diagnostics::err::TypeMismatch);
        return error_type;
    }

    const TypeId resolved_object = resolve(object);
    TypeId element               = error_type;
    uint64_t object_length       = 0;
    const bool is_array          = type_table.kindOf(resolved_object) == TypeKind::Array;
    const bool is_slice          = type_table.kindOf(resolved_object) == TypeKind::Slice;
    const TypeId pointer_type    = pointerBase(resolved_object);
    bool is_pointer              = pointer_type != kInvalidTypeId;
    if (is_array) {
        if (const auto *array = type_table.array(resolved_object)) {
            element       = array->element;
            object_length = array->size;
        }
    } else if (is_slice) {
        if (const auto *slice = type_table.slice(resolved_object))
            element = slice->element;
    } else if (is_pointer && expr.is_raw) {
        // A raw pointer slice creates a view over C-owned storage. Checked
        // pointer slicing is intentionally not added: there is no length field
        // to validate against unless the caller supplies the bound explicitly.
        if (const auto *ptr = type_table.pointer(pointer_type))
            element = ptr->pointee;
    } else {
        report(expr.span, "slice target must be an array or slice", diagnostics::err::TypeMismatch);
        return error_type;
    }

    if (is_array && !expr.is_raw) {
        // Static known bounds are rejected before any runtime code is generated.
        int64_t lo          = 0;
        int64_t hi          = 0;
        const bool lo_known = constantIntegerValue(expr.operands[1], lo);
        const bool hi_known = constantIntegerValue(expr.operands[2], hi);
        if (lo_known && hi_known) {
            if (lo < 0 || static_cast<uint64_t>(hi) > object_length || lo > hi) {
                report(expr.span, "slice bounds are outside the array or reversed",
                       diagnostics::err::TypeMismatch);
                return error_type;
            }
        }
    }

    const TypeId slice_type = type_table.internSlice(element);
    return expr.is_raw ? slice_type : type_table.internOptional(slice_type);
}
TypeId PerModuleSema::inferField(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    // Fully-qualified module paths (`std.io.console.println`, `std.counter.Counter`)
    // keep intermediate Field nodes whose base ultimately binds to a module alias.
    // Those nodes are namespaces, not value fields: sema must not diagnose them as
    // missing members. The final member is resolved as an Import by the session pass.
    if (const auto *own_resolved = findResolvedExpr(id);
        own_resolved != nullptr && own_resolved->kind == session::ResolutionKind::Import) {
        if (const TypeId imported_type = typeOfResolvedName(id))
            return imported_type;
    } else {
        const frontend::Expression *base = &expr;
        unsigned guard                   = 0;
        while (guard++ < 16U) {
            if (base->kind == frontend::ExprKind::Name) {
                const auto *binding = findResolvedBinding(base->text, base->scope);
                if (binding != nullptr && binding->kind == session::ResolutionKind::ModuleAlias)
                    return error_type;
                break;
            }
            if (base->kind != frontend::ExprKind::Field || base->operands.empty())
                break;
            if (base->operands[0].value > snapshot.expressions().size())
                break;
            base = &snapshot.expressions()[base->operands[0].value - 1U];
        }
    }
    // `console.println` where `console` is an import alias: the resolution pass binds the
    // field expression to the imported symbol, so resolve that before touching the base
    // (which would report "unknown identifier 'console'").
    if (const auto *resolved = findResolvedExpr(id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::Import) {
        if (const TypeId imported_type = typeOfResolvedName(id))
            return imported_type;
        report(expr.span, "imported member '" + expr.text + "' has no known type",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    // `Color.Green` on a name that resolves to an enum declaration is a variant access,
    // not a struct field access. Inside an enum discriminant it has the enum's
    // underlying integer type so it composes with arithmetic; after the declaration
    // is lowered it retains the enum type (as before).
    if (const auto enum_type = enumVariantType(expr.operands[0], expr.text, expr.span)) {
        const auto *base = findResolvedExpr(expr.operands[0]);
        if (base != nullptr) {
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Enum || decl.name != base->name)
                    continue;
                bool inside_enum_default = false;
                for (const auto &variant : decl.parameters) {
                    if (!variant.defaultValue ||
                        variant.defaultValue.value > snapshot.expressions().size())
                        continue;
                    const auto &default_expr =
                        snapshot.expressions()[variant.defaultValue.value - 1U];
                    if (expr.span.start >= default_expr.span.start &&
                        expr.span.end <= default_expr.span.end) {
                        inside_enum_default = true;
                        break;
                    }
                }
                if (inside_enum_default) {
                    const TypeId underlying =
                        decl.declaredType ? lowerTypeExpr(decl.declaredType) : i32_type;
                    if (underlying && type_table.kindOf(resolve(underlying)) == TypeKind::Integer) {
                        return underlying;
                    }
                }
                break;
            }
        }
        return *enum_type;
    }
    // A struct name is a type, not a value. Reject `Pair.first` before the base
    // is treated as an expression that lowerings can silently drop.
    if (const auto *resolved = findResolvedExpr(expr.operands[0]);
        resolved != nullptr && resolved->kind == session::ResolutionKind::Declaration &&
        resolved->declaration && resolved->declKind == frontend::DeclKind::Struct) {
        report(expr.span,
               "struct name '" + resolved->name + "' cannot be used as a value in field access;" +
                   " use a value such as 'p.first'",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    TypeId object_type = inferExpr(expr.operands[0]);
    TypeId resolved    = resolve(object_type);
    // `self.field` is canonical for an implicit `*Owner` receiver. Sema treats
    // it like the legacy `self->field`; lowering still emits a deref/HirField.
    if (type_table.kindOf(resolved) == TypeKind::Pointer &&
        (isSelfReceiver(expr.operands[0]) || isBorrowParameter(expr.operands[0]))) {
        if (const auto *ptr = type_table.pointer(resolved))
            resolved = resolve(ptr->pointee);
    }
    const auto *st = type_table.struct_type(resolved);
    if (st == nullptr) {
        if (const auto *pack = type_table.pack(resolved)) {
            for (size_t index = 0; index < pack->names.size(); ++index) {
                if (pack->names[index] == expr.text)
                    return pack->members[index];
            }
            report(expr.span, "unknown pack member '" + expr.text + "'",
                   diagnostics::err::NoMember);
            return error_type;
        }
        if (type_table.kindOf(resolved) == TypeKind::GenericParam) {
            for (const TypeId bound : boundsForGenericParam(resolved)) {
                if (!isInterfaceType(bound))
                    continue;
                const auto *trait_ty = type_table.trait(resolve(bound));
                const auto *iface =
                    trait_ty != nullptr
                        ? findDeclNamed(trait_ty->name, frontend::DeclKind::Interface)
                        : nullptr;
                if (iface == nullptr)
                    continue;
                for (const auto &required : iface->parameters) {
                    if (required.name != expr.text)
                        continue;
                    const TypeId field_type = lowerTypeExprConst(required.type);
                    if (!field_type)
                        break;
                    return field_type;
                }
            }
            report(expr.span,
                   "unknown field '" + expr.text + "' on generic parameter '" +
                       type_table.typeToString(object_type) + "'",
                   diagnostics::err::NoMember);
            return error_type;
        }
        report(expr.span,
               "field access on non-struct type having type '" +
                   type_table.typeToString(object_type) + "'",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    int idx = type_table.fieldIndex(resolved, expr.text);
    if (idx < 0) {
        report(expr.span,
               "unknown field '" + expr.text + "' on type '" +
                   type_table.typeToString(object_type) + "'",
               diagnostics::err::NoMember);
        return error_type;
    }
    if (!fieldVisible(*st, static_cast<size_t>(idx))) {
        report(expr.span,
               "field '" + expr.text + "' of type '" + type_table.typeToString(object_type) +
                   "' is private; use a public accessor",
               diagnostics::err::NoMember);
        return error_type;
    }
    return st->fields[static_cast<size_t>(idx)];
}
memory::Optional<TypeId> PerModuleSema::enumVariantType(frontend::ExprId operand,
                                                        std::string_view variant,
                                                        frontend::TextSpan span) {
    if (!operand || operand.value > snapshot.expressions().size())
        return {};
    const auto &op_expr = snapshot.expressions()[operand.value - 1U];
    if (op_expr.kind != frontend::ExprKind::Name)
        return {};
    // Only a name that resolves to the enum *declaration* qualifies; a value of enum
    // type (`value.Green`) must fall through to the regular field-access diagnostic.
    const TypeId name_type = typeOfResolvedName(operand);
    if (!name_type)
        return {};
    const TypeId resolved = resolve(name_type);
    const auto *et        = type_table.enum_type(resolved);
    if (et == nullptr)
        return {};
    for (size_t i = 0; i < et->variant_names.size(); ++i) {
        if (et->variant_names[i] == variant)
            return resolved;
    }
    report(span, "unknown enum variant '" + std::string(variant) + "'", diagnostics::err::NoMember);
    return {};
}
TypeId PerModuleSema::inferArrow(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId ptr_type = inferExpr(expr.operands[0]);
    TypeId resolved = resolve(ptr_type);
    // `?*T` is accepted here: the niche representation is the bare pointer. Flow-sensitive
    // narrowing after `is null` is not implemented yet, so this is unchecked.
    if (type_table.kindOf(resolved) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(resolved))
            resolved = resolve(opt->inner);
    }
    if (type_table.kindOf(resolved) != TypeKind::Pointer) {
        report(expr.span, "'->' requires a pointer operand", diagnostics::err::TypeMismatch);
        return error_type;
    }
    const auto *ptr = type_table.pointer(resolved);
    if (ptr == nullptr)
        return error_type;
    TypeId struct_type = resolve(ptr->pointee);
    const auto *st     = type_table.struct_type(struct_type);
    if (st == nullptr) {
        report(expr.span,
               "'->' on a pointer to non-struct type '" + type_table.typeToString(ptr->pointee) +
                   "'",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    int idx = type_table.fieldIndex(struct_type, expr.text);
    if (idx < 0) {
        report(expr.span,
               "unknown field '" + expr.text + "' on type '" +
                   type_table.typeToString(struct_type) + "'",
               diagnostics::err::NoMember);
        return error_type;
    }
    if (!fieldVisible(*st, static_cast<size_t>(idx))) {
        report(expr.span,
               "field '" + expr.text + "' of type '" + type_table.typeToString(struct_type) +
                   "' is private; use a public accessor",
               diagnostics::err::NoMember);
        return error_type;
    }
    return st->fields[static_cast<size_t>(idx)];
}
bool PerModuleSema::fieldVisible(const StructType &st, size_t field_index) const noexcept {
    const auto &meta = field_index < st.field_meta.size()
                           ? st.field_meta[field_index]
                           : FieldMeta{frontend::Visibility::Private, 0, module};
    if (meta.visibility == frontend::Visibility::Public)
        return true;
    if (meta.visibility == frontend::Visibility::Private)
        return meta.owner.empty() || module == meta.owner;

    // Module visibility is file-relative in this compiler: the declaring file
    // is always allowed, and a different file is allowed when it is at most
    // `modDepth` directories below the module that owns the struct. A negative
    // depth (`mod(..)`) means unlimited depth.
    if (meta.owner.empty() || module == meta.owner)
        return true;
    if (meta.modDepth < 0)
        return true;

    const std::string_view current_path = module;
    const std::string_view owner_path   = meta.owner;
    const auto owner_dir                = owner_path.substr(0, owner_path.find_last_of('/'));
    if (!current_path.starts_with(owner_dir) || owner_dir.empty())
        return false;
    const auto relative = current_path.substr(owner_dir.size() + 1U);
    int32_t depth       = 0;
    for (const char ch : relative) {
        if (ch == '/')
            ++depth;
    }
    return depth <= meta.modDepth;
}

} // namespace zith::sema::modern
