#include "sema/hir-lower-utils.hpp"
#include "sema/op-mapping.hpp"
#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <algorithm>
#include <limits>

namespace zith::sema::modern {

TypeId PerModuleSema::inferExprExpected(frontend::ExprId id, TypeId expected) {
    if (!expected)
        return inferExpr(id);
    // Only a direct call carries a result-only generic parameter worth
    // contextual inference. Other expression kinds keep the plain path so the
    // expectation cannot perturb an unrelated inference. A nested call reached
    // through the call's own operands must not inherit this expectation.
    const TypeId saved = expectedResultType_;
    expectedResultType_ = expected;
    const TypeId result = inferExpr(id);
    expectedResultType_ = saved;
    return result;
}

TypeId PerModuleSema::inferExpr(frontend::ExprId id) {
    if (!id)
        return error_type;
    // Capture the expected result type a surrounding context recorded for this
    // expression, then clear it so it is consumed exactly once and never leaks
    // into a nested callee's own inference.
    const TypeId expected_result = expectedResultType_;
    expectedResultType_          = kInvalidTypeId;
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
    case frontend::ExprKind::Pipe:
    case frontend::ExprKind::PipeDo:
        result = inferPipe(id);
        break;
    case frontend::ExprKind::PipeCurrent:
        result = inferPipeCurrent(id);
        break;
    case frontend::ExprKind::Call:
        result = inferCall(id, expected_result);
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
        result = inferPackLiteral(id, expected_result);
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
    case frontend::ExprKind::FunctionLiteral: {
        // The literal's value is the synthesized function's address, so its
        // type is exactly the function type of that declaration.
        const TypeId fn_type = expr.declaration ? typeOfDecl(expr.declaration) : kInvalidTypeId;
        result               = fn_type ? fn_type : error_type;
        break;
    }
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
    if (id.value <= snapshot.expressions().size() &&
        snapshot.expressions()[id.value - 1U].kind == frontend::ExprKind::Range) {
        report(span,
               "a literal range is not a boolean condition; write 'x in range' or a loop "
               "variable, e.g. 'for (x in 1..5)'",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
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
    if (looksString(text)) {
        checkLiteralEscapes(id, text);
        return type_table.internPointer(char_type);
    }
    if (looksChar(text)) {
        checkLiteralEscapes(id, text);
        return char_type;
    }
    return error_type;
}

bool PerModuleSema::checkLiteralEscapes(frontend::ExprId id, std::string_view text) {
    // Both `looksString` and `looksChar` require the matching delimiter, so the
    // body is the source text between the quotes.
    const bool is_char = text.front() == '\'';
    const std::string_view body(text.data() + 1U, text.size() - 2U);
    std::string decoded;
    if (!decodeEscapes(body, decoded)) {
        if (id && id.value <= snapshot.expressions().size()) {
            report(snapshot.expressions()[id.value - 1U].span,
                   is_char ? "invalid escape sequence in char literal"
                           : "invalid escape sequence in string literal",
                   diagnostics::err::InvalidEscape);
        }
        return false;
    }
    // A character literal must decode to exactly one character. Checking it
    // here keeps `check` consistent with HIR lowering, which rejects a wider
    // body such as `'ab'`.
    if (is_char && decoded.size() != 1U) {
        if (id && id.value <= snapshot.expressions().size()) {
            report(snapshot.expressions()[id.value - 1U].span,
                   "char literal must contain exactly one character or escape",
                   diagnostics::err::InvalidEscape);
        }
        return false;
    }
    return true;
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
        const auto *declaration = declarationForResolved(*resolved);
        const bool raw_function =
            resolved->foreignFunction != nullptr ||
            (declaration != nullptr && declaration->kind == frontend::DeclKind::Function &&
             declaration->functionKind == frontend::FunctionKind::Raw);
        if (raw_function) {
            const auto &expression = snapshot.expressions()[id.value - 1U];
            report(expression.span, "raw function cannot be used as a function value",
                   diagnostics::err::UnsupportedSyntax);
            return error_type;
        }
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
    if (const TypeId generic_param = genericParamTypeByName(text)) {
        const TypeId generic_resolved = resolve(generic_param);
        if (type_table.kindOf(generic_resolved) == TypeKind::GenericParam)
            return generic_param;
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
        if (type_table.kindOf(resolved) == TypeKind::Optional) {
            const auto *opt = type_table.optional(resolved);
            if (opt != nullptr && type_table.kindOf(resolve(opt->inner)) == TypeKind::Pointer &&
                exprHasNonNullPointerProof(expr.operands[0])) {
                const auto *ptr = type_table.pointer(resolve(opt->inner));
                result          = ptr ? ptr->pointee : error_type;
            } else if (opt != nullptr &&
                       type_table.kindOf(resolve(opt->inner)) == TypeKind::Pointer) {
                report(expr.span,
                       "cannot dereference a possibly-null pointer; narrow it after 'is null' or "
                       "use 'raw' to bypass the check",
                       diagnostics::err::NullDerefUnproven);
                result = error_type;
            } else {
                report(expr.span, "unary '*' expects a pointer operand",
                       diagnostics::err::TypeMismatch);
                result = error_type;
            }
        } else if (type_table.kindOf(resolved) != TypeKind::Pointer) {
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
    TypeId left = inferExpr(expr.operands[0]);
    if (expr.text == "in")
        return inferContains(expr.id, expr.operands[0], expr.operands[1], expr.span);
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

struct PipeUseRange {
    size_t minimum = 0U;
    size_t maximum = 0U;
};

static size_t addPipeUseCounts(size_t left, size_t right) {
    if (left == std::numeric_limits<size_t>::max() ||
        right > std::numeric_limits<size_t>::max() - left)
        return std::numeric_limits<size_t>::max();
    return left + right;
}

static PipeUseRange sequencePipeUses(PipeUseRange left, PipeUseRange right) {
    return {addPipeUseCounts(left.minimum, right.minimum),
            addPipeUseCounts(left.maximum, right.maximum)};
}

static PipeUseRange branchPipeUses(PipeUseRange left, PipeUseRange right) {
    return {std::min(left.minimum, right.minimum), std::max(left.maximum, right.maximum)};
}

static PipeUseRange pipeCurrentUses(frontend::ExprId id,
                                    const frontend::FrontendSnapshot &snapshot,
                                    const std::vector<uint32_t> &borrowed_uses) {
    if (!id || id.value > snapshot.expressions().size())
        return {};
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::PipeCurrent) {
        const bool borrowed =
            std::find(borrowed_uses.begin(), borrowed_uses.end(), expr.id.value) !=
            borrowed_uses.end();
        return borrowed ? PipeUseRange{} : PipeUseRange{1U, 1U};
    }
    if (expr.kind == frontend::ExprKind::FunctionLiteral)
        return {};

    if (expr.kind == frontend::ExprKind::If && expr.operands.size() >= 2U) {
        PipeUseRange condition = pipeCurrentUses(expr.operands[0], snapshot, borrowed_uses);
        const PipeUseRange then_branch =
            pipeCurrentUses(expr.operands[1], snapshot, borrowed_uses);
        PipeUseRange else_branch{};
        if (expr.operands.size() == 3U) {
            else_branch = pipeCurrentUses(expr.operands[2], snapshot, borrowed_uses);
        } else if (expr.operands.size() >= 4U) {
            else_branch =
                sequencePipeUses(pipeCurrentUses(expr.operands[2], snapshot, borrowed_uses),
                                 pipeCurrentUses(expr.operands[3], snapshot, borrowed_uses));
        }
        return sequencePipeUses(condition, branchPipeUses(then_branch, else_branch));
    }

    if (expr.kind == frontend::ExprKind::Binary &&
        (expr.text == "and" || expr.text == "or") && expr.operands.size() == 2U) {
        const PipeUseRange left = pipeCurrentUses(expr.operands[0], snapshot, borrowed_uses);
        const PipeUseRange right = pipeCurrentUses(expr.operands[1], snapshot, borrowed_uses);
        return sequencePipeUses(left, branchPipeUses({}, right));
    }

    if (expr.kind == frontend::ExprKind::When && !expr.operands.empty()) {
        PipeUseRange branches{};
        PipeUseRange tested_conditions{};
        bool has_branch = false;
        bool has_default = false;
        for (size_t index = 1U; index < expr.operands.size(); ++index) {
            const size_t condition_index = index - 1U;
            const frontend::ExprId condition =
                condition_index < expr.conditions.size() ? expr.conditions[condition_index]
                                                         : frontend::ExprId{};
            if (condition) {
                tested_conditions = sequencePipeUses(
                    tested_conditions, pipeCurrentUses(condition, snapshot, borrowed_uses));
            } else {
                has_default = true;
            }
            const PipeUseRange path =
                sequencePipeUses(tested_conditions,
                                 pipeCurrentUses(expr.operands[index], snapshot, borrowed_uses));
            branches = has_branch ? branchPipeUses(branches, path) : path;
            has_branch = true;
        }
        if (!has_default)
            branches = has_branch ? branchPipeUses(branches, tested_conditions)
                                  : tested_conditions;
        return sequencePipeUses(pipeCurrentUses(expr.operands[0], snapshot, borrowed_uses),
                                branches);
    }

    if (expr.kind == frontend::ExprKind::While || expr.kind == frontend::ExprKind::For ||
        expr.kind == frontend::ExprKind::ForIn) {
        PipeUseRange loop_uses{};
        for (const auto operand : expr.operands)
            loop_uses =
                sequencePipeUses(loop_uses, pipeCurrentUses(operand, snapshot, borrowed_uses));
        if (loop_uses.maximum != 0U)
            return {0U, std::numeric_limits<size_t>::max()};
        return loop_uses;
    }

    PipeUseRange count{};
    for (const auto operand : expr.operands)
        count = sequencePipeUses(count, pipeCurrentUses(operand, snapshot, borrowed_uses));
    for (const auto stmt_id : expr.statements) {
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (stmt.expression)
            count = sequencePipeUses(
                count, pipeCurrentUses(stmt.expression, snapshot, borrowed_uses));
        if (stmt.kind == frontend::StmtKind::Binding && stmt.binding.initializer &&
            stmt.binding.initializer != stmt.expression)
            count = sequencePipeUses(
                count, pipeCurrentUses(stmt.binding.initializer, snapshot, borrowed_uses));
    }
    return count;
}

static bool containsNestedPipe(frontend::ExprId id,
                               const frontend::FrontendSnapshot &snapshot) {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::FunctionLiteral)
        return false;
    if (expr.kind == frontend::ExprKind::Pipe || expr.kind == frontend::ExprKind::PipeDo)
        return true;
    for (const auto operand : expr.operands) {
        if (containsNestedPipe(operand, snapshot))
            return true;
    }
    for (const auto stmt_id : expr.statements) {
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (containsNestedPipe(stmt.expression, snapshot) ||
            (stmt.kind == frontend::StmtKind::Binding &&
             containsNestedPipe(stmt.binding.initializer, snapshot)))
            return true;
    }
    return false;
}

bool PerModuleSema::containsPipeCurrent(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::PipeCurrent)
        return true;
    if (expr.kind == frontend::ExprKind::FunctionLiteral)
        return false;
    for (const auto operand : expr.operands) {
        if (containsPipeCurrent(operand))
            return true;
    }
    for (const auto stmt_id : expr.statements) {
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (containsPipeCurrent(stmt.expression) ||
            (stmt.kind == frontend::StmtKind::Binding &&
             containsPipeCurrent(stmt.binding.initializer)))
            return true;
    }
    return false;
}

void PerModuleSema::markPipeCurrentBorrowed(frontend::ExprId id) {
    if (!id || id.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::PipeCurrent) {
        if (pipeBorrowedUses_ != nullptr)
            pipeBorrowedUses_->push_back(expr.id.value);
        return;
    }
    if (expr.kind == frontend::ExprKind::FunctionLiteral)
        return;
    for (const auto operand : expr.operands)
        markPipeCurrentBorrowed(operand);
}

TypeId PerModuleSema::inferPipe(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() != 2U)
        return error_type;

    const TypeId source = inferExpr(expr.operands[0]);
    if (containsNestedPipe(expr.operands[1], snapshot)) {
        report(snapshot.expressions()[expr.operands[1].value - 1U].span,
               "pipeline stages cannot contain nested pipelines",
               diagnostics::err::UnsupportedSyntax);
        return error_type;
    }
    const auto current  = pipeCurrentType_;
    const bool was_observing = pipeObserving_;
    std::vector<uint32_t> borrowed_uses;
    auto *const previous_borrowed_uses = pipeBorrowedUses_;
    pipeCurrentType_         = source;
    pipeObserving_           = expr.kind == frontend::ExprKind::PipeDo;
    pipeBorrowedUses_        = &borrowed_uses;
    const TypeId stage_type = inferExpr(expr.operands[1]);
    pipeCurrentType_        = current;
    pipeObserving_          = was_observing;
    pipeBorrowedUses_        = previous_borrowed_uses;

    const PipeUseRange current_uses =
        pipeCurrentUses(expr.operands[1], snapshot, borrowed_uses);
    switch (expr.kind) {
    case frontend::ExprKind::Pipe:
        if (current_uses.minimum == 0U && current_uses.maximum == 0U) {
            report(expr.span,
                   "pipeline stage must consume 'it' exactly once on every execution path",
                   diagnostics::err::ExpectedExpr);
            return error_type;
        }
        if (current_uses.minimum != 1U || current_uses.maximum != 1U) {
            report(expr.span, "pipeline stage must consume 'it' exactly once on every execution path",
                   diagnostics::err::UnsupportedSyntax);
            return error_type;
        }
        break;
    case frontend::ExprKind::PipeDo:
        break;
    default:
        return error_type;
    }
    if (expr.kind == frontend::ExprKind::PipeDo) {
        const auto &stage = snapshot.expressions()[expr.operands[1].value - 1U];
        if (stage.kind == frontend::ExprKind::Block && deferBodyHasControlFlow(expr.operands[1])) {
            report(stage.span,
                   "'also { ... }' cannot transfer control out of the observing stage",
                   diagnostics::err::UnsupportedSyntax);
        }
        return source;
    }
    return stage_type;
}

TypeId PerModuleSema::inferPipeCurrent(frontend::ExprId id) {
    if (pipeCurrentType_ == kInvalidTypeId) {
        if (id && id.value <= snapshot.expressions().size()) {
            const auto &expr = snapshot.expressions()[id.value - 1U];
            report(expr.span, "'it' is only valid inside a '|>' or 'also' pipeline stage",
                   diagnostics::err::UnsupportedSyntax);
        }
        return error_type;
    }
    return pipeCurrentType_;
}

TypeId PerModuleSema::inferContains(frontend::ExprId binary_id, frontend::ExprId value,
                                    frontend::ExprId rhs, frontend::TextSpan span) {
    if (!value || !rhs)
        return error_type;
    if (rhs.value <= snapshot.expressions().size()) {
        const auto &rhs_node = snapshot.expressions()[rhs.value - 1U];
        if (rhs_node.kind == frontend::ExprKind::Range) {
            if (rhs_node.operands.size() != 2U)
                return error_type;
            const TypeId lo = inferExpr(rhs_node.operands[0]);
            const TypeId hi = inferExpr(rhs_node.operands[1]);
            if (lo == error_type || hi == error_type)
                return error_type;
            if (!sameType(lo, hi) && !adaptNumericLiteral(rhs_node.operands[1], lo)) {
                report(span, "range bounds must have the same type",
                       diagnostics::err::TypeMismatch);
                return error_type;
            }
            const TypeId value_type = inferExpr(value);
            const TypeId bound      = resolve(type_table.stripQualifiers(lo));
            if (!sameType(value_type, bound) && !adaptNumericLiteral(value, bound)) {
                report(span, "'in' value type does not match the range bound type",
                       diagnostics::err::TypeMismatch);
                return error_type;
            }
            typed_map.containsCall.erase(binary_id.value);
            return bool_type;
        }
    }
    const TypeId rhs_type = inferExpr(rhs);
    if (!rhs_type || type_table.kindOf(resolve(rhs_type)) == TypeKind::Error)
        return error_type;

    TypeId pointee = resolve(type_table.stripQualifiers(rhs_type));
    if (type_table.kindOf(pointee) == TypeKind::Pointer) {
        if (const auto *ptr = type_table.pointer(pointee))
            pointee = resolve(type_table.stripQualifiers(ptr->pointee));
    } else if (type_table.kindOf(pointee) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(pointee)) {
            pointee = resolve(type_table.stripQualifiers(opt->inner));
        }
    }

    const auto methods = findMethodsForOwner(ownerNameOf(pointee), "contains");
    const frontend::Declaration *contains_decl = nullptr;
    session::ModuleKey contains_module;
    for (const auto &method : methods) {
        if (method.decl == nullptr || method.decl->parameters.size() < 2U ||
            method.decl->parameters[0].name != "self")
            continue;
        contains_decl   = method.decl;
        contains_module = method.module;
        break;
    }
    if (contains_decl == nullptr)
        report(span, "'in' requires a range RHS or a type with 'contains(self, value): bool'",
               diagnostics::err::TypeMismatch);
    else {
        const PerModuleSema *contains_sema =
            owner != nullptr ? owner->findModuleSema(contains_module) : nullptr;
        const TypeId fn_type = contains_sema != nullptr
                                   ? contains_sema->typeOfDecl(contains_decl->id)
                                   : typeOfDecl(contains_decl->id);
        const auto *fn       = type_table.function(fn_type);
        if (fn == nullptr || fn->params.size() < 2U)
            return error_type;

        const TypeId value_type = inferExpr(value);
        if (!coerceValue(value, fn->params[1], value_type))
            report(span, "'in' value type does not match the 'contains' parameter",
                   diagnostics::err::TypeMismatch);
        if (!sameType(resolve(fn->result), bool_type)) {
            report(span, "Contains method 'contains' must return bool",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        typed_map.containsCall.insert(binary_id.value,
                                      TypedMap::ContainsCall{contains_module, contains_decl->id});
        return bool_type;
    }
    return error_type;
}

} // namespace zith::sema::modern
