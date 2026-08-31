#include "sema/op-mapping.hpp"
#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <algorithm>

namespace zith::sema::modern {

TypeId PerModuleSema::inferExpr(frontend::ExprId id) {
    if (!id)
        return error_type;
    if (TypeId existing = typeOfExpr(id); existing)
        return existing;
    if (id.value > snapshot.expressions().size())
        return error_type;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(id);
            resolved != nullptr && resolved->local &&
            movedLocals_.contains(resolved->local.value) && !containedInRawRead(id)) {
            report(expr.span,
                   "cannot use '" + expr.text + "' after it was moved by a previous call",
                   diagnostics::err::UseAfterMove);
        }
        if (const auto *resolved = findResolvedExpr(id);
            resolved != nullptr && resolved->local &&
            uninitializedLocals_.contains(resolved->local.value) && !expr.isRawName) {
            report(expr.span,
                   "binding '" + expr.text +
                       "' is used before it is initialized; assign a value first or use 'raw'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    TypeId result;
    switch (expr.kind) {
    case frontend::ExprKind::OwnershipCoerce:
        // An annotation only narrows how the operand is passed. The value
        // keeps the inner type for overload selection and later coercion, so
        // generic inference and overload resolution see the pointee rather
        // than the borrow ABI pointer.
        if (expr.operands.empty()) {
            result = error_type;
        } else {
            result = inferExpr(expr.operands[0]);
            if (result && type_table.kindOf(result) == TypeKind::Pointer) {
                if (const auto *pointer = type_table.pointer(result); pointer != nullptr)
                    result = type_table.stripQualifiers(pointer->pointee);
            }
        }
        break;
    case frontend::ExprKind::DockCall:
        inferDockCall(id);
        result = typeOfExpr(id);
        break;
    case frontend::ExprKind::Literal:
        result = inferLiteral(id, expr.text);
        break;
    case frontend::ExprKind::Name:
        result = inferName(id, expr.text);
        break;
    case frontend::ExprKind::Unary:
        result = inferUnary(id);
        break;
    case frontend::ExprKind::Binary:
        result = inferBinary(id);
        break;
    case frontend::ExprKind::Call:
        result = inferCall(id);
        break;
    case frontend::ExprKind::Block:
        result = inferBlock(id);
        break;
    case frontend::ExprKind::If:
        result = inferIf(id);
        break;
    case frontend::ExprKind::While:
        result = inferWhile(id);
        break;
    case frontend::ExprKind::For:
        result = inferFor(id);
        break;
    case frontend::ExprKind::ForIn:
        result = inferForIn(id);
        break;
    case frontend::ExprKind::Return:
        result = inferReturn(id);
        break;
    case frontend::ExprKind::Assign:
        result = inferAssign(id);
        break;
    case frontend::ExprKind::OptionalProp:
        result = inferOptionalProp(id);
        break;
    case frontend::ExprKind::Index:
        result = inferIndex(id);
        break;
    case frontend::ExprKind::SliceRange:
        result = inferSliceRange(id);
        break;
    case frontend::ExprKind::Field:
        result = inferField(id);
        break;
    case frontend::ExprKind::Arrow:
        result = inferArrow(id);
        break;
    case frontend::ExprKind::StructLiteral:
        result = inferStructLiteral(id);
        break;
    case frontend::ExprKind::PackLiteral:
        result = inferPackLiteral(id);
        break;
    case frontend::ExprKind::ArrayLiteral:
        result = inferArrayLiteral(id);
        break;
    case frontend::ExprKind::Cast:
        result = inferCast(id);
        break;
    case frontend::ExprKind::IsNull:
        result = inferIsNull(id);
        break;
    case frontend::ExprKind::IsType:
        result = inferIsType(id);
        break;
    case frontend::ExprKind::When:
        result = inferWhen(id);
        break;
    case frontend::ExprKind::Range:
        result = inferRange(id);
        break;
    case frontend::ExprKind::Placeholder:
        // `_` gets its type from the struct field it fills; the literal checks it.
        result = error_type;
        break;
    case frontend::ExprKind::LayoutIntrinsic:
        result = inferLayoutIntrinsic(id);
        break;
    case frontend::ExprKind::MacroCall:
        // The expansion is reached through the call site, so infer it here
        // rather than assuming an earlier pass already typed it; otherwise a
        // binding fed by `@macro()` would take an invalid type.
        result = expr.expansion ? inferExpr(expr.expansion) : error_type;
        break;
    default:
        result = error_type;
        break;
    }
    setExprType(id, result);
    return result;
}
TypeId PerModuleSema::inferCondition(frontend::ExprId id, std::string_view message,
                                     frontend::TextSpan span) {
    if (!id)
        return error_type;
    const TypeId source = inferExpr(id);
    if (sameType(source, bool_type))
        return source;
    if (type_table.optional(resolve(source)) != nullptr)
        return source;
    report(span, std::string(message), diagnostics::err::TypeMismatch);
    return error_type;
}
TypeId PerModuleSema::inferLiteral(frontend::ExprId id, std::string_view text) {
    if (text == "null")
        return null_type;
    if (looksBool(text))
        return bool_type;
    if (looksFloat(text))
        return f64_type;
    std::int64_t parsed [[maybe_unused]] = 0;
    switch (support::parseIntegerLiteral(text, parsed)) {
    case support::IntLiteralStatus::Ok: {
        const auto suffix = support::integerSuffix(text);
        if (!suffix.empty()) {
            const TypeId suffix_type = type_table.lookupNamed(suffix);
            if (suffix_type)
                return suffix_type;
        }
        return i32_type;
    }
    case support::IntLiteralStatus::Overflow:
        // A literal wider than 64 bits has no representable type; diagnose it rather than
        // truncating it during HIR lowering.
        if (id && id.value <= snapshot.expressions().size()) {
            report(snapshot.expressions()[id.value - 1U].span,
                   "integer literal '" + std::string(text) + "' does not fit in 64 bits",
                   diagnostics::err::InvalidIntLiteral);
        }
        return error_type;
    case support::IntLiteralStatus::NotInteger:
        break;
    }
    if (looksString(text))
        return type_table.internPointer(char_type);
    if (looksChar(text))
        return char_type;
    return error_type;
}
TypeId PerModuleSema::inferName(frontend::ExprId id, std::string_view text) {
    // Inside an enum discriminant, a bare name may refer to a variant declared earlier in
    // the same enum. The variants are constants of the enum's underlying type for the
    // purpose of the constant evaluator and expression typing.
    if (id && id.value <= snapshot.expressions().size()) {
        const auto &expr = snapshot.expressions()[id.value - 1U];
        if (expr.scope) {
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Enum)
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
                if (!inside_enum_default)
                    continue;
                for (const auto &variant : decl.parameters) {
                    if (variant.name == text) {
                        const TypeId underlying =
                            decl.declaredType ? lowerTypeExpr(decl.declaredType) : i32_type;
                        if (underlying &&
                            type_table.kindOf(resolve(underlying)) == TypeKind::Integer) {
                            return underlying;
                        }
                        break;
                    }
                }
            }
        }
    }
    const auto *resolved = findResolvedExpr(id);
    if (resolved) {
        if (const TypeId resolved_type = typeOfResolvedName(id)) {
            if (const TypeId recorded = typeOfExpr(id); recorded) {
                return recorded;
            }
            if (resolve(resolved_type) == invalid_type) {
                const auto &resolved_expr = snapshot.expressions()[id.value - 1U];
                report(resolved_expr.span,
                       "cannot infer type of '" + std::string(text) +
                           "'; assign a value before reading it or add a type annotation",
                       diagnostics::err::CannotInfer);
                return error_type;
            }
            const TypeId known_type = resolved_type;
            return known_type;
        }
        // A bare import/module alias has no value type; it is only valid as the base of a
        // field access (`console.println`), which the resolution pass binds separately.
        if (resolved->kind == session::ResolutionKind::ModuleAlias)
            return error_type;
    }
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.scope) {
        TypeId local = typeOfLocalByName(expr.scope, text);
        if (local)
            return local;
    }
    // No resolution available — diagnose the unbound name.
    report(expr.span, "unknown identifier '" + std::string(text) + "'",
           diagnostics::err::UndefinedIdent);
    return error_type;
}
TypeId PerModuleSema::inferUnary(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId result;
    TypeId operand = inferExpr(expr.operands[0]);
    if (expr.text == "must") {
        const TypeId resolved = resolve(operand);
        const auto *opt       = type_table.optional(resolved);
        if (opt == nullptr) {
            report(expr.span, "'must' expects an optional operand", diagnostics::err::TypeMismatch);
            return error_type;
        }
        return type_table.stripQualifiers(opt->inner);
    }
    if (expr.text == "raw") {
        const TypeId resolved = resolve(operand);
        if (const auto *opt = type_table.optional(resolved)) {
            // `raw x` on an optional bypasses the null check and extracts its
            // payload without proving it is present.
            return type_table.stripQualifiers(opt->inner);
        }
        // Other `raw x` usages are the explicit unchecked read escape; they
        // preserve the inner type.
        return operand;
    }
    if (expr.text == "not") {
        if (!sameType(operand, bool_type))
            report(expr.span, "unary 'not' expects a boolean operand",
                   diagnostics::err::TypeMismatch);
        result = bool_type;
    } else if (expr.text == "-") {
        if (!sameType(operand, i32_type) && !sameType(operand, i64_type) &&
            !sameType(operand, f32_type) && !sameType(operand, f64_type)) {
            report(expr.span, "unary '-' expects a numeric operand",
                   diagnostics::err::TypeMismatch);
        }
        result = operand;
    } else if (expr.text == "~") {
        if (type_table.integer(resolve(operand)) == nullptr) {
            report(expr.span, "unary '~' expects an integer operand",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else {
            result = operand;
        }
    } else if (expr.text == "&") {
        // Address-of moves the binding logically and produces a pointer object.
        // The pointer is tied to the storage scope and cannot escape it.
        if (!expr.operands.empty()) {
            const auto &operand_expr = snapshot.expressions()[expr.operands[0].value - 1U];
            if (const auto *resolved = findResolvedExpr(expr.operands[0]);
                resolved != nullptr && resolved->local) {
                movedLocals_.insert(resolved->local.value);
            }
            if (operand_expr.kind == frontend::ExprKind::Name)
                escapingPointerExprs_.insert(id.value);
        }
        result = type_table.internPointer(operand);
    } else if (expr.text == "*") {
        // Dereference: operand must be a pointer
        TypeId resolved = resolve(operand);
        if (type_table.kindOf(resolved) != TypeKind::Pointer) {
            report(expr.span, "unary '*' expects a pointer operand",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else {
            const auto *ptr = type_table.pointer(resolved);
            result          = ptr ? ptr->pointee : error_type;
        }
    } else {
        result = error_type;
    }
    return result;
}
TypeId PerModuleSema::inferBinary(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    TypeId result;
    TypeId left  = inferExpr(expr.operands[0]);
    TypeId right = inferExpr(expr.operands[1]);
    // A numeric literal on either side takes the other side's type.
    if (!sameType(left, right)) {
        if (adaptNumericLiteral(expr.operands[1], left))
            right = left;
        else if (adaptNumericLiteral(expr.operands[0], right))
            left = right;
    }
    if (sema::isComparisonOp(expr.text)) {
        if (!sameType(left, right))
            report(expr.span, "comparison between incompatible types",
                   diagnostics::err::TypeMismatch);
        result = bool_type;
    } else if (sema::isShiftOp(expr.text) || sema::isBitwiseOp(expr.text)) {
        // Both operands must be integers (after the numeric-literal adaptation above) and
        // share the same type, matching LLVM's same-type CreateShl/CreateAShr requirement.
        const bool left_integer          = type_table.integer(resolve(left)) != nullptr;
        const bool right_integer         = type_table.integer(resolve(right)) != nullptr;
        const std::string_view kind_name = sema::isShiftOp(expr.text) ? "shift" : "bitwise";
        if (!left_integer || !right_integer) {
            report(expr.span, std::string(kind_name) + " operator expects integer operands",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else if (!sameType(left, right)) {
            report(expr.span, std::string(kind_name) + " operands have incompatible types",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else {
            result = left;
        }
    } else if (expr.text == "and" || expr.text == "or" || expr.text == "xor") {
        if (!sameType(left, bool_type) || !sameType(right, bool_type)) {
            if (expr.text == "and" || expr.text == "or") {
                report(expr.span, "operator '" + expr.text + "' expects boolean operands",
                       diagnostics::err::TypeMismatch);
                result = error_type;
            } else {
                const bool left_int  = type_table.integer(resolve(left)) != nullptr;
                const bool right_int = type_table.integer(resolve(right)) != nullptr;
                if ((left_int && right_int && sameType(left, right)) ||
                    (sameType(left, bool_type) && sameType(right, bool_type))) {
                    result = left;
                } else {
                    report(expr.span,
                           "operator 'xor' expects boolean operands or integers of the same type",
                           diagnostics::err::TypeMismatch);
                    result = error_type;
                }
            }
        } else {
            result = bool_type;
        }
    } else if (sema::isArithmeticOp(expr.text)) {
        if (!sameType(left, right))
            report(expr.span, "arithmetic between incompatible types",
                   diagnostics::err::TypeMismatch);
        result = left;
    } else if (expr.text == "=") {
        if (!sameType(left, right))
            report(expr.span, "assignment between incompatible types",
                   diagnostics::err::TypeMismatch);
        result = left;
    } else {
        result = error_type;
    }
    return result;
}

} // namespace zith::sema::modern
