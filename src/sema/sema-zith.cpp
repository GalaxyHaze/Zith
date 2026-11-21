#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace zith::sema::modern {

namespace {

bool containsExplicitAlignmentByValue(const TypeTable &types, const TypeId root) {
    std::vector<TypeId> pending{root};
    std::vector<bool> visited(types.size(), false);

    while (!pending.empty()) {
        const TypeId type = types.stripQualifiers(types.canonical(pending.back()));
        pending.pop_back();
        if (!type || type.intern_seq > types.size())
            continue;
        const size_t index = static_cast<size_t>(type.intern_seq - 1U);
        if (visited[index])
            continue;
        visited[index] = true;

        switch (types.kindOf(type)) {
        case TypeKind::Array:
            if (const auto *array = types.array(type); array != nullptr)
                pending.push_back(array->element);
            break;
        case TypeKind::Optional:
            if (const auto *optional = types.optional(type); optional != nullptr)
                pending.push_back(optional->inner);
            break;
        case TypeKind::Failable:
            if (const auto *failable = types.failable(type); failable != nullptr)
                pending.push_back(failable->inner);
            break;
        case TypeKind::Struct:
            if (const auto *structure = types.struct_type(type); structure != nullptr) {
                if (structure->explicit_alignment != 0U)
                    return true;
                for (const TypeId field : structure->fields)
                    pending.push_back(field);
            }
            break;
        case TypeKind::Union:
            if (const auto *union_type = types.union_type(type); union_type != nullptr)
                for (const TypeId member : union_type->members)
                    pending.push_back(member);
            break;
        case TypeKind::Sum:
            if (const auto *sum = types.sum(type); sum != nullptr)
                for (const TypeId member : sum->members)
                    pending.push_back(member);
            break;
        case TypeKind::Pack:
            if (const auto *pack = types.pack(type); pack != nullptr)
                for (const TypeId member : pack->members)
                    pending.push_back(member);
            break;
        default:
            break;
        }
    }
    return false;
}

} // namespace

bool PerModuleSema::alignmentIntegerValue(const frontend::ExprId id, std::int64_t &out,
                                          const unsigned depth) {
    if (!id || id.value > snapshot.expressions().size() || depth > 64U)
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Literal)
        return support::parseIntegerLiteral(expr.text, out) == support::IntLiteralStatus::Ok;
    if (expr.kind == frontend::ExprKind::Unary && expr.operands.size() == 1U) {
        std::int64_t operand = 0;
        if (!alignmentIntegerValue(expr.operands.front(), operand, depth + 1U))
            return false;
        if (expr.text == "+") {
            out = operand;
            return true;
        }
        if (expr.text == "-" && operand != std::numeric_limits<std::int64_t>::min()) {
            out = -operand;
            return true;
        }
        if (expr.text == "~") {
            out = ~operand;
            return true;
        }
        return false;
    }
    if (expr.kind == frontend::ExprKind::Binary && expr.operands.size() == 2U) {
        std::int64_t left  = 0;
        std::int64_t right = 0;
        if (!alignmentIntegerValue(expr.operands[0], left, depth + 1U) ||
            !alignmentIntegerValue(expr.operands[1], right, depth + 1U))
            return false;
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if (expr.text == "+") {
            if ((right > 0 && left > max - right) || (right < 0 && left < min - right))
                return false;
            out = left + right;
            return true;
        }
        if (expr.text == "-") {
            if ((right < 0 && left > max + right) || (right > 0 && left < min + right))
                return false;
            out = left - right;
            return true;
        }
        if (expr.text == "*") {
            if (left == 0 || right == 0) {
                out = 0;
                return true;
            }
            if ((left > 0 && right > 0 && left > max / right) ||
                (left > 0 && right < 0 && right < min / left) ||
                (left < 0 && right > 0 && left < min / right) ||
                (left < 0 && right < 0 && left < max / right))
                return false;
            out = left * right;
            return true;
        }
        if ((expr.text == "/" || expr.text == "%") && right != 0 && !(left == min && right == -1)) {
            out = expr.text == "/" ? left / right : left % right;
            return true;
        }
        if ((expr.text == "<<" || expr.text == ">>") && right >= 0 && right < 63) {
            if (expr.text == ">>") {
                out = left >> static_cast<unsigned>(right);
                return true;
            }
            if (left >= 0 && left <= (max >> static_cast<unsigned>(right))) {
                out = left << static_cast<unsigned>(right);
                return true;
            }
            return false;
        }
        if (expr.text == "&" || expr.text == "|") {
            out = expr.text == "&" ? left & right : left | right;
            return true;
        }
        if (expr.text == "^") {
            out = left ^ right;
            return true;
        }
    }
    if (expr.kind == frontend::ExprKind::Name && isConstantExpression(id)) {
        const auto *resolved = findResolvedExpr(id);
        if (resolved == nullptr || resolved->bindingKind != frontend::BindingKind::Const)
            return false;
        const TypeId const_type = typeOfResolvedName(id);
        if (!const_type || type_table.integer(type_table.stripQualifiers(const_type)) == nullptr)
            return false;
        if (resolved->declaration &&
            resolved->declaration.value <= snapshot.declarations().size()) {
            const auto &decl = snapshot.declarations()[resolved->declaration.value - 1U];
            return alignmentIntegerValue(decl.initializer, out, depth + 1U);
        }
        for (const auto &statement : snapshot.statements()) {
            if (statement.kind == frontend::StmtKind::Binding &&
                statement.binding.id == resolved->local)
                return alignmentIntegerValue(statement.binding.initializer, out, depth + 1U);
        }
    }
    return false;
}

bool PerModuleSema::getRequestedAlignment(const frontend::ExprId id, uint32_t &out) {
    std::int64_t value = 0;
    if (!id || !alignmentIntegerValue(id, value) || value <= 0 ||
        value > std::numeric_limits<uint32_t>::max())
        return false;

    const auto alignment = static_cast<uint32_t>(value);
    if ((alignment & (alignment - 1U)) != 0U)
        return false;
    out = alignment;
    return true;
}

uint32_t PerModuleSema::requestedStructAlignment(const frontend::Declaration &decl) {
    for (const auto &attribute : decl.attributes) {
        if (attribute.kind != frontend::AttributeKind::AlignAs)
            continue;
        uint32_t alignment = 0;
        if (getRequestedAlignment(attribute.argument, alignment))
            return alignment;
    }
    return 0U;
}

void PerModuleSema::checkAlignmentAttributes() {
    const auto validate = [&](const frontend::Attribute &attribute) {
        if (attribute.kind != frontend::AttributeKind::AlignAs)
            return;
        uint32_t alignment = 0;
        if (!attribute.argument || !getRequestedAlignment(attribute.argument, alignment)) {
            report(attribute.span,
                   "attribute 'alignas' requires a positive power-of-two integer constant "
                   "independent of generic parameters and representable by the compiler",
                   diagnostics::err::UnsupportedSyntax);
        }
    };

    for (const auto &decl : snapshot.declarations()) {
        for (const auto &attribute : decl.attributes)
            validate(attribute);
        if (decl.kind == frontend::DeclKind::Variable && decl.isExtern) {
            for (const auto &attribute : decl.attributes) {
                if (attribute.kind == frontend::AttributeKind::AlignAs) {
                    report(attribute.span,
                           "attribute 'alignas' cannot guarantee alignment for external global "
                           "storage",
                           diagnostics::err::UnsupportedSyntax);
                }
            }
        }
        if (decl.kind == frontend::DeclKind::Function &&
            (decl.isExtern || !decl.externalSymbol.empty())) {
            const auto *function = type_table.function(typeOfDecl(decl.id));
            if (function != nullptr) {
                bool has_unverified_aligned_value =
                    containsExplicitAlignmentByValue(type_table, function->result);
                for (const TypeId parameter : function->params)
                    has_unverified_aligned_value |=
                        containsExplicitAlignmentByValue(type_table, parameter);
                if (has_unverified_aligned_value) {
                    report(decl.span,
                           "unsupported C ABI: explicitly aligned values by value require a "
                           "verified target layout",
                           diagnostics::err::UnsupportedSyntax);
                }
            }
        }
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        const auto has_noalias =
            std::find_if(decl.attributes.begin(), decl.attributes.end(), [](const auto &attribute) {
                return attribute.kind == frontend::AttributeKind::NoAlias;
            });
        if (has_noalias == decl.attributes.end())
            continue;
        const auto *function = type_table.function(typeOfDecl(decl.id));
        if (function == nullptr || type_table.kindOf(type_table.stripQualifiers(
                                       resolve(function->result))) != TypeKind::Pointer) {
            report(has_noalias->span, "attribute 'noalias' requires a pointer return type",
                   diagnostics::err::UnsupportedSyntax);
        }
    }

    for (const auto &statement : snapshot.statements()) {
        for (const auto &attribute : statement.binding.attributes)
            validate(attribute);
        if (statement.kind != frontend::StmtKind::Binding) {
            for (const auto &attribute : statement.attributes)
                validate(attribute);
        }
    }
}

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
        // An imported name carries its source binding metadata in the
        // resolution; the provider's symbol id is not an index into this
        // module's declaration snapshot.
        if (resolved->kind == session::ResolutionKind::Import)
            return resolved->declKind == frontend::DeclKind::Variable &&
                   resolved->bindingKind == frontend::BindingKind::Const;
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
void PerModuleSema::checkTestDeclarations() {
    frontend::TextSpan first_test_span{};
    bool has_test = false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || !decl.isTest())
            continue;
        if (!has_test)
            first_test_span = decl.span;
        has_test = true;
        // A test is a free function: no owner, no parent, no generics.
        if (!decl.parameters.empty()) {
            reportWarning(decl.span, "a #[test] function cannot declare parameters",
                          diagnostics::err::AttributeNotApplicable);
        }
        const TypeId fn_type = typeOfDecl(decl.id);
        if (const auto *fn = type_table.function(fn_type);
            fn != nullptr && fn->result &&
            type_table.kindOf(resolve(fn->result)) != TypeKind::Void &&
            type_table.kindOf(resolve(fn->result)) != TypeKind::Error) {
            reportWarning(decl.span, "a #[test] function must return void",
                          diagnostics::err::AttributeNotApplicable);
        }
    }
    if (!has_test)
        return;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Function && decl.name == "main" &&
            decl.ownerName.empty() && decl.parentName.empty()) {
            report(first_test_span, "a test module cannot declare both 'main' and #[test]",
                   diagnostics::err::UnsupportedSyntax);
            return;
        }
    }
}

void PerModuleSema::checkZithDeclarations() {
    checkTestDeclarations();
    // Unknown `as`/`for` targets are deferred from the parser to sema so the
    // frontend can still represent cross-module implementations before imports
    // are resolved. Unknown targets are not nominal traits or interfaces.
    for (const auto &record : snapshot.implementRecords()) {
        frontend::DeclKind trait_kind = frontend::DeclKind::Error;
        const frontend::Declaration *trait =
            findDeclNamed(record.traitName, frontend::DeclKind::Trait, nullptr);
        if (trait != nullptr) {
            trait_kind = frontend::DeclKind::Trait;
        } else {
            trait = findDeclNamed(record.traitName, frontend::DeclKind::Interface, nullptr);
            if (trait != nullptr)
                trait_kind = frontend::DeclKind::Interface;
        }
        if (trait == nullptr) {
            report(record.span, "'" + record.traitName + "' is not a declared trait or interface",
                   diagnostics::err::NotATrait);
        } else if (trait_kind == frontend::DeclKind::Interface) {
            report(record.span, "interfaces are structural and cannot be implemented explicitly",
                   diagnostics::err::InterfaceMethodNotAllowed);
        }
    }

    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Variable)
            continue;
        TypeId declared = typeOfDecl(decl.id);
        if (!declared)
            declared = error_type;
        if (decl.bindingKind == frontend::BindingKind::Const && !decl.isExtern) {
            if (!decl.initializer) {
                report(decl.span, "Zith--: const declaration requires an initializer",
                       diagnostics::err::UnsupportedSyntax);
            } else if (!isConstantExpression(decl.initializer)) {
                report(snapshot.expressions()[decl.initializer.value - 1U].span,
                       "Zith--: const initializer must be a constant expression",
                       diagnostics::err::UnsupportedSyntax);
            }
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
        const bool is_for_in_binding = std::find(for_in_bindings.begin(), for_in_bindings.end(),
                                                 binding.id) != for_in_bindings.end();
        if (!binding.initializer && !binding.type && !is_for_in_binding &&
            type_table.kindOf(resolve(local_type)) == TypeKind::Invalid) {
            report(binding.span, "binding requires a type annotation or an initializer",
                   diagnostics::err::CannotInfer);
            continue;
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
    // Type-level probes still need to know that `?*T` can be narrowed to the
    // checked pointer target; the expression-level proof lives in `coerceValue`.
    return isNullablePointer(resolve(source)) &&
           type_table.kindOf(resolve(target)) == TypeKind::Pointer &&
           sameType(target, pointerBase(resolve(source)));
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
        } else if (type_table.kindOf(resolved_target) == TypeKind::Opaque) {
            // Any concrete value erases into bare `opaque`. `coerceValue`
            // records the concrete source so HIR can build the tagged payload,
            // and the `?opaque` target reaches here through the recursive
            // optional case above.
            result = type_table.kindOf(resolved_source) != TypeKind::Opaque &&
                     resolved_source != null_type && resolved_source != void_type;
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
        if (sa == nullptr || sb == nullptr)
            return false;
        const auto base = [](std::string_view name) {
            if (const size_t angle = name.find('<'); angle != std::string_view::npos)
                return name.substr(0, angle);
            return name;
        };
        if (base(sa->name) != base(sb->name))
            return false;
        if (!sa->args.empty() && sa->args.size() == sb->args.size()) {
            for (size_t index = 0; index < sa->args.size(); ++index) {
                if (!sameType(sa->args[index], sb->args[index]))
                    return false;
            }
        }
        return true;
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
