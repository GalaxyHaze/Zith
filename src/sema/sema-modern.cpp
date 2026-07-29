#include "sema/sema-modern.hpp"

#include "diagnostics/error-codes.hpp"

#include <cstring>

#include <cctype>
#include <cstdlib>
#include <string>

namespace zith::sema::modern {

namespace {

bool looksInteger(std::string_view text) {
    if (text.empty())
        return false;
    size_t i = 0;
    if (text[0] == '-' || text[0] == '+')
        i++;
    for (; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return false;
    }
    return i > (text[0] == '-' || text[0] == '+' ? 1u : 0u);
}

bool looksFloat(std::string_view text) {
    if (text.empty())
        return false;
    bool saw_dot = false;
    size_t i     = 0;
    if (text[0] == '-' || text[0] == '+')
        i++;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '.') {
            if (saw_dot)
                return false;
            saw_dot = true;
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return saw_dot;
}

bool looksBool(std::string_view text) {
    return text == "true" || text == "false";
}

bool looksString(std::string_view text) {
    return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}

bool isComparisonOp(std::string_view text) {
    return text == "==" || text == "!=" || text == "<" || text == ">" || text == "<=" ||
           text == ">=";
}

bool isArithmeticOp(std::string_view text) {
    return text == "+" || text == "-" || text == "*" || text == "/" || text == "%";
}

/// The single decision point for `as` conversions. User-defined casts will become a new
/// branch here rather than a new call site.
enum class CastKind : uint8_t { Invalid, Identity, IntToInt, IntToFloat, FloatToInt, FloatToFloat };

[[nodiscard]] CastKind classifyCast(TypeKind from, TypeKind to) {
    if (from == to && (from == TypeKind::Integer || from == TypeKind::Float))
        return from == TypeKind::Integer ? CastKind::IntToInt : CastKind::FloatToFloat;
    if (from == TypeKind::Integer && to == TypeKind::Float)
        return CastKind::IntToFloat;
    if (from == TypeKind::Float && to == TypeKind::Integer)
        return CastKind::FloatToInt;
    return CastKind::Invalid;
}

} // namespace

PerModuleSema::PerModuleSema(session::ModuleKey mod, const frontend::FrontendSnapshot &snap,
                             const session::ModuleResolution &res, TypeTable &tt, TypedMap &tm,
                             memory::Arena &a, SemaPipeline *owner_)
    : module(std::move(mod)), snapshot(snap), resolution(res), type_table(tt), typed_map(tm),
      arena(a), diagnostics(a), owner(owner_), error_type(kInvalidTypeId),
      void_type(kInvalidTypeId), bool_type(kInvalidTypeId), char_type(kInvalidTypeId),
      i32_type(kInvalidTypeId), i64_type(kInvalidTypeId), f32_type(kInvalidTypeId),
      f64_type(kInvalidTypeId) {}

bool PerModuleSema::run() {
    if (!prepareTypes())
        return false;
    if (!checkExpressions())
        return false;
    return !hasErrors();
}

bool PerModuleSema::prepareTypes() {
    registerPrimitiveTypes();
    registerNamedTypes();
    lowerDeclarationTypes();
    return true;
}

bool PerModuleSema::checkExpressions() {
    inferExpressionTypes();
    checkReturnsAndCalls();
    return !hasErrors();
}

bool PerModuleSema::hasErrors() const noexcept {
    for (const auto &d : diagnostics)
        if (d.severity == diagnostics::Severity::Error)
            return true;
    return false;
}

TypeId PerModuleSema::typeOfExpr(frontend::ExprId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.exprTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

TypeId PerModuleSema::typeOfDecl(frontend::DeclId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.declTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

TypeId PerModuleSema::typeOfLocal(frontend::LocalId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.localTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

void PerModuleSema::setExprType(frontend::ExprId id, TypeId type) {
    if (id)
        typed_map.exprTypes.insert(id.value, type);
}

void PerModuleSema::setDeclType(frontend::DeclId id, TypeId type) {
    if (id)
        typed_map.declTypes.insert(id.value, type);
}

void PerModuleSema::setLocalType(frontend::LocalId id, TypeId type) {
    if (id)
        typed_map.localTypes.insert(id.value, type);
}

void PerModuleSema::report(frontend::TextSpan span, std::string message, uint32_t code) {
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Error, code);
}

void PerModuleSema::reportNote(frontend::TextSpan span, std::string message) {
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Note,
                        static_cast<uint32_t>(0));
}

void PerModuleSema::registerPrimitiveTypes() {
    error_type = type_table.internName("error", TypeKind::Error);
    void_type  = type_table.internName("void", TypeKind::Void);
    bool_type  = type_table.internName("bool", TypeKind::Bool);
    char_type  = type_table.internName("char", TypeKind::Char);
    i32_type   = type_table.internInteger({32, true});
    i64_type   = type_table.internInteger({64, true});
    f32_type   = type_table.internFloat({32});
    f64_type   = type_table.internFloat({64});
    type_table.registerNamed("void", void_type);
    type_table.registerNamed("bool", bool_type);
    type_table.registerNamed("char", char_type);
    type_table.registerNamed("i32", i32_type);
    type_table.registerNamed("i64", i64_type);
    type_table.registerNamed("f32", f32_type);
    type_table.registerNamed("f64", f64_type);
    null_type = type_table.internName("null", TypeKind::Never);
}

void PerModuleSema::registerNamedTypes() {
    for (const auto &decl : snapshot.declarations()) {
        switch (decl.kind) {
        case frontend::DeclKind::Struct:
            (void)type_table.findOrCreateNamed(decl.name, TypeKind::Struct);
            break;
        case frontend::DeclKind::Enum:
            (void)type_table.findOrCreateNamed(decl.name, TypeKind::Enum);
            break;
        case frontend::DeclKind::Union:
            (void)type_table.findOrCreateNamed(decl.name, TypeKind::Union);
            break;
        case frontend::DeclKind::Trait:
            (void)type_table.findOrCreateNamed(decl.name, TypeKind::Trait);
            break;
        case frontend::DeclKind::TypeAlias:
            (void)type_table.findOrCreateNamed(decl.name, TypeKind::Alias);
            break;
        default:
            break;
        }
    }
}

void PerModuleSema::lowerDeclarationTypes() {
    for (const auto &decl : snapshot.declarations()) {
        switch (decl.kind) {
        case frontend::DeclKind::Function: {
            auto &params_storage = type_table.makeTypeStorage();
            for (const auto &param : decl.parameters) {
                TypeId ptype = lowerTypeExpr(param.type);
                if (!ptype)
                    ptype = error_type;
                setLocalType(param.id, ptype);
                params_storage.push(ptype);
            }
            TypeId result = lowerTypeExpr(decl.declaredType);
            if (!result)
                result = void_type;
            TypeId fn_type = type_table.internFunction(params_storage, result);
            setDeclType(decl.id, fn_type);
            break;
        }
        case frontend::DeclKind::Variable: {
            TypeId vtype = lowerTypeExpr(decl.declaredType);
            if (!vtype)
                vtype = error_type;
            setDeclType(decl.id, vtype);
            break;
        }
        case frontend::DeclKind::TypeAlias: {
            TypeId target = lowerTypeExpr(decl.declaredType);
            if (target) {
                TypeId alias = type_table.internAlias(target);
                setDeclType(decl.id, alias);
                type_table.registerNamed(decl.name, alias);
            }
            break;
        }
        case frontend::DeclKind::Struct: {
            auto &fields    = type_table.makeTypeStorage();
            auto &fld_names = type_table.makeStringStorage();
            // Intern each field's name (as arena string_view) and type
            for (const auto &param : decl.parameters) {
                TypeId ftype = lowerTypeExpr(param.type);
                if (!ftype)
                    ftype = error_type;
                fields.push(ftype);
                // Store name in a stable arena allocation
                char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
                std::memcpy(buf, param.name.data(), param.name.size());
                fld_names.push(std::string_view(buf, param.name.size()));
            }
            TypeId st = type_table.internStruct(decl.name, fields, &fld_names);
            setDeclType(decl.id, st);
            type_table.registerNamed(decl.name, st);
            break;
        }
        case frontend::DeclKind::Enum: {
            auto &variants = type_table.makeTypeStorage();
            TypeId et      = type_table.internEnum(decl.name, variants);
            setDeclType(decl.id, et);
            type_table.registerNamed(decl.name, et);
            break;
        }
        case frontend::DeclKind::Union: {
            auto &members = type_table.makeTypeStorage();
            TypeId ut     = type_table.internUnion(decl.name, members);
            setDeclType(decl.id, ut);
            type_table.registerNamed(decl.name, ut);
            break;
        }
        case frontend::DeclKind::Trait: {
            TypeId tt = type_table.internTrait(decl.name);
            setDeclType(decl.id, tt);
            type_table.registerNamed(decl.name, tt);
            break;
        }
        default:
            break;
        }
    }
}

void PerModuleSema::inferExpressionTypes() {
    inferExpressionTypesForDecls();
}

void PerModuleSema::inferExpressionTypesForDecls() {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Function) {
            TypeId fn_type     = typeOfDecl(decl.id);
            const auto *fn     = type_table.function(fn_type);
            currentReturnType_ = fn ? fn->result : kInvalidTypeId;
        } else {
            currentReturnType_ = kInvalidTypeId;
        }
        if (decl.body) {
            (void)inferExpr(decl.body);
        }
        if (decl.initializer) {
            (void)inferExpr(decl.initializer);
        }
    }
    // Also infer standalone expressions
    for (const auto &expr : snapshot.expressions()) {
        if (!typeOfExpr(expr.id))
            (void)inferExpr(expr.id);
    }
}

void PerModuleSema::checkReturnsAndCalls() {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        TypeId fn_type = typeOfDecl(decl.id);
        const auto *fn = type_table.function(fn_type);
        if (!fn)
            continue;
        TypeId ret_type = fn->result;
        if (decl.body) {
            TypeId body_type = typeOfExpr(decl.body);
            if (!sameType(body_type, void_type) && ret_type != void_type &&
                !coercesTo(ret_type, body_type)) {
                reportCoercionFailure(snapshot.expressions()[decl.body.value - 1U].span, ret_type,
                                      body_type,
                                      "function body type does not match declared return type");
            }
        }
    }

    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind == frontend::ExprKind::Return) {
            // Return type is checked during inferReturn.
        } else if (expr.kind == frontend::ExprKind::Call) {
            TypeId call_type = typeOfExpr(expr.id);
            (void)call_type;
        }
    }
}

TypeId PerModuleSema::lowerTypeExpr(frontend::TypeExprId id) noexcept {
    // `*void` / `?*void` are rejected here (the TypeTable has no diagnostics channel).
    // `raw opaque` remains the supported spelling for C interop.
    if (id && id.value <= snapshot.typeExpressions().size()) {
        const auto &type = snapshot.typeExpressions()[id.value - 1U];
        if ((type.kind == frontend::TypeExprKind::Pointer ||
             type.kind == frontend::TypeExprKind::Optional) &&
            !type.arguments.empty()) {
            const auto arg = type.arguments[0];
            if (arg && arg.value <= snapshot.typeExpressions().size()) {
                const auto &inner = snapshot.typeExpressions()[arg.value - 1U];
                if (inner.kind == frontend::TypeExprKind::Name && inner.name == "void") {
                    report(type.span,
                           "pointer to 'void' is not allowed; use 'raw opaque' for C interop");
                    return error_type;
                }
            }
        }
    }
    return type_table.lowerTypeExpr(snapshot, id);
}

TypeId PerModuleSema::lowerForeignType(const cinterop::Type &type) {
    switch (type.kind) {
    case cinterop::TypeKind::Void:
        return void_type;
    case cinterop::TypeKind::Bool:
        return bool_type;
    case cinterop::TypeKind::Integer:
        return type_table.internInteger({type.bits, type.isSigned});
    case cinterop::TypeKind::Float:
        return type_table.internFloat({type.bits});
    case cinterop::TypeKind::Pointer:
        return type_table.internPointer(type.pointee ? lowerForeignType(*type.pointee)
                                                     : error_type);
    case cinterop::TypeKind::Record:
        return type_table.findOrCreateNamed(type.name, TypeKind::Struct);
    case cinterop::TypeKind::Enum:
        return type_table.findOrCreateNamed(type.name, TypeKind::Enum);
    }
    return error_type;
}

TypeId PerModuleSema::inferExpr(frontend::ExprId id) {
    if (!id)
        return error_type;
    if (TypeId existing = typeOfExpr(id); existing)
        return existing;
    if (id.value > snapshot.expressions().size())
        return error_type;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId result;
    switch (expr.kind) {
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
    case frontend::ExprKind::Field:
        result = inferField(id);
        break;
    case frontend::ExprKind::Arrow:
        result = inferArrow(id);
        break;
    case frontend::ExprKind::StructLiteral:
        result = inferStructLiteral(id);
        break;
    case frontend::ExprKind::Cast:
        result = inferCast(id);
        break;
    case frontend::ExprKind::IsNull:
        result = inferIsNull(id);
        break;
    default:
        result = error_type;
        break;
    }
    setExprType(id, result);
    return result;
}

TypeId PerModuleSema::inferLiteral(frontend::ExprId id, std::string_view text) {
    (void)id;
    if (text == "null")
        return null_type;
    if (looksBool(text))
        return bool_type;
    if (looksFloat(text))
        return f64_type;
    if (looksInteger(text))
        return i32_type;
    if (looksString(text))
        return type_table.internPointer(char_type);
    return error_type;
}

TypeId PerModuleSema::inferName(frontend::ExprId id, std::string_view text) {
    (void)text;
    const auto *resolved = findResolvedExpr(id);
    if (resolved) {
        TypeId type = typeOfResolvedName(id);
        if (type)
            return type;
    }
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.scope)
        return typeOfLocalByName(expr.scope, text);
    return error_type;
}

TypeId PerModuleSema::inferUnary(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId result;
    TypeId operand = inferExpr(expr.operands[0]);
    if (expr.text == "not" || expr.text == "!") {
        if (!sameType(operand, bool_type))
            report(expr.span, "unary 'not' expects a boolean operand");
        result = bool_type;
    } else if (expr.text == "-") {
        if (!sameType(operand, i32_type) && !sameType(operand, i64_type) &&
            !sameType(operand, f32_type) && !sameType(operand, f64_type)) {
            report(expr.span, "unary '-' expects a numeric operand");
        }
        result = operand;
    } else if (expr.text == "&") {
        // Address-of: produce pointer to operand type
        result = type_table.internPointer(operand);
    } else if (expr.text == "*") {
        // Dereference: operand must be a pointer
        TypeId resolved = resolve(operand);
        if (type_table.kindOf(resolved) != TypeKind::Pointer) {
            report(expr.span, "unary '*' expects a pointer operand");
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
    if (isComparisonOp(expr.text)) {
        if (!sameType(left, right))
            report(expr.span, "comparison between incompatible types");
        result = bool_type;
    } else if (isArithmeticOp(expr.text)) {
        if (!sameType(left, right))
            report(expr.span, "arithmetic between incompatible types");
        result = left;
    } else if (expr.text == "=") {
        if (!sameType(left, right))
            report(expr.span, "assignment between incompatible types");
        result = left;
    } else {
        result = error_type;
    }
    return result;
}

TypeId PerModuleSema::inferCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId callee_type = inferExpr(expr.operands[0]);
    const auto *fn     = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function");
        return error_type;
    }
    size_t arg_count = expr.operands.size() - 1;
    if (arg_count != fn->params.size()) {
        report(expr.span, "function call arity mismatch");
        return fn->result;
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
        TypeId arg_type = inferExpr(expr.operands[i + 1]);
        if (!coerceValue(expr.operands[i + 1], fn->params[i], arg_type))
            reportCoercionFailure(expr.span, fn->params[i], arg_type,
                                  "function call argument type mismatch");
    }
    return fn->result;
}

TypeId PerModuleSema::inferBlock(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId last      = void_type;
    for (const auto &stmt_id : expr.statements) {
        if (!stmt_id)
            continue;
        if (stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (stmt.kind == frontend::StmtKind::Expression && stmt.expression) {
            last = inferExpr(stmt.expression);
        } else if (stmt.kind == frontend::StmtKind::Binding) {
            TypeId init_type = inferExpr(stmt.binding.initializer);
            TypeId ann_type  = lowerTypeExpr(stmt.binding.type);
            if (ann_type && stmt.binding.initializer &&
                !coerceValue(stmt.binding.initializer, ann_type, init_type)) {
                reportCoercionFailure(stmt.span, ann_type, init_type,
                                      "binding initializer type does not match annotation");
            }
            if (!ann_type && resolve(init_type) == null_type) {
                report(stmt.span, "null requires an optional type annotation");
                init_type = error_type;
            }
            setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Return) {
            if (stmt.expression)
                (void)inferExpr(stmt.expression);
            last = void_type;
        }
    }
    return last;
}

TypeId PerModuleSema::inferIf(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    TypeId cond = inferExpr(expr.operands[0]);
    if (!sameType(cond, bool_type))
        report(expr.span, "if condition must be boolean");
    TypeId then_type = inferExpr(expr.operands[1]);
    TypeId else_type = expr.operands.size() >= 3 ? inferExpr(expr.operands[2]) : void_type;
    if (sameType(then_type, else_type))
        return then_type;
    return then_type;
}

TypeId PerModuleSema::inferWhile(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (!expr.operands.empty()) {
        TypeId cond = inferExpr(expr.operands[0]);
        if (!sameType(cond, bool_type))
            report(expr.span, "loop condition must be boolean");
    }
    // The body must be inferred too, otherwise locals declared inside the loop never get a type.
    if (expr.operands.size() >= 2U)
        (void)inferExpr(expr.operands[1]);
    return void_type;
}

TypeId PerModuleSema::inferCast(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    const TypeId source = inferExpr(expr.operands[0]);
    const TypeId target = lowerTypeExpr(expr.cast_type);
    TypeId result       = target;
    if (!target) {
        report(expr.span, "unknown target type in 'as' conversion");
        result = error_type;
    } else if (source && source != error_type) {
        const CastKind kind =
            classifyCast(type_table.kindOf(resolve(source)), type_table.kindOf(resolve(target)));
        if (kind == CastKind::Invalid) {
            report(expr.span, "'as' only supports numeric conversions",
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
    const TypeId operand = inferExpr(expr.operands[0]);
    if (operand == error_type || !operand)
        return error_type;
    if (type_table.kindOf(resolve(operand)) != TypeKind::Optional) {
        report(expr.span, "'is null' requires an optional operand ('?T')");
        return error_type;
    }
    return bool_type;
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
    setExprType(value, target);
    return true;
}

bool PerModuleSema::coerceValue(frontend::ExprId value, TypeId target, TypeId source) {
    if (coercesTo(target, source)) {
        // Record the optional target on a `null` literal so lowering can emit None directly.
        if (resolve(source) == null_type &&
            type_table.kindOf(resolve(target)) == TypeKind::Optional) {
            setExprType(value, target);
        }
        return true;
    }
    return adaptNumericLiteral(value, target);
}

void PerModuleSema::reportCoercionFailure(frontend::TextSpan span, TypeId target, TypeId source,
                                          std::string_view context) {
    if (resolve(source) == null_type) {
        report(span, "cannot assign 'null' to a non-optional pointer; use '?*T'");
        return;
    }
    const TypeKind from = type_table.kindOf(resolve(source));
    const TypeKind to   = type_table.kindOf(resolve(target));
    if (classifyCast(from, to) != CastKind::Invalid) {
        report(span, "implicit numeric conversion is not allowed; use 'as'");
        return;
    }
    report(span, std::string(context));
}

TypeId PerModuleSema::inferReturn(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId value     = expr.operands.empty() ? void_type : inferExpr(expr.operands[0]);
    if (currentReturnType_ && value && value != error_type && !expr.operands.empty() &&
        !coerceValue(expr.operands[0], currentReturnType_, value)) {
        reportCoercionFailure(expr.span, currentReturnType_, value,
                              "return type does not match declared return type");
    }
    return type_table.internName("never", TypeKind::Never);
}

TypeId PerModuleSema::inferAssign(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    TypeId left_type  = inferExpr(expr.operands[0]);
    TypeId right_type = inferExpr(expr.operands[1]);
    TypeId result     = left_type;
    if (!coerceValue(expr.operands[1], left_type, right_type)) {
        reportCoercionFailure(expr.span, left_type, right_type,
                              "assignment between incompatible types");
        result = error_type;
    }
    return result;
}

TypeId PerModuleSema::inferOptionalProp(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId operand = inferExpr(expr.operands[0]);
    if (operand == error_type || !operand)
        return error_type;
    TypeId resolved = resolve(operand);
    if (type_table.kindOf(resolved) != TypeKind::Optional) {
        report(expr.span, "'?' operator requires an optional operand");
        return error_type;
    }
    const auto *opt = type_table.optional(resolved);
    if (!opt)
        return error_type;
    // Verify enclosing function returns an optional that can accept this inner type
    if (currentReturnType_) {
        TypeId ret_resolved = resolve(currentReturnType_);
        if (type_table.kindOf(ret_resolved) != TypeKind::Optional) {
            report(expr.span, "'?' operator used in a function that does not return an optional");
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
            report(expr.span, "array index must be an integer");
        const TypeId resolved_object = resolve(object);
        switch (type_table.kindOf(resolved_object)) {
        case TypeKind::Slice:
            if (const auto *slice = type_table.slice(resolved_object))
                result = slice->element;
            break;
        case TypeKind::Array:
            if (const auto *array = type_table.array(resolved_object))
                result = array->element;
            break;
        case TypeKind::Pointer:
            if (const auto *pointer = type_table.pointer(resolved_object))
                result = pointer->pointee;
            break;
        case TypeKind::Error:
            break;
        default:
            report(expr.span, "type is not indexable");
            break;
        }
    }
    return result;
}

TypeId PerModuleSema::inferField(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId object_type = inferExpr(expr.operands[0]);
    TypeId resolved    = resolve(object_type);
    const auto *st     = type_table.struct_type(resolved);
    if (st == nullptr) {
        report(expr.span, "field access on non-struct type");
        return error_type;
    }
    int idx = type_table.fieldIndex(resolved, expr.text);
    if (idx < 0) {
        report(expr.span, "unknown field '" + expr.text + "'");
        return error_type;
    }
    return st->fields[static_cast<size_t>(idx)];
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
        report(expr.span, "'->' requires a pointer operand");
        return error_type;
    }
    const auto *ptr = type_table.pointer(resolved);
    if (ptr == nullptr)
        return error_type;
    TypeId struct_type = resolve(ptr->pointee);
    const auto *st     = type_table.struct_type(struct_type);
    if (st == nullptr) {
        report(expr.span, "'->' on a pointer to non-struct type");
        return error_type;
    }
    int idx = type_table.fieldIndex(struct_type, expr.text);
    if (idx < 0) {
        report(expr.span, "unknown field '" + expr.text + "'");
        return error_type;
    }
    return st->fields[static_cast<size_t>(idx)];
}

TypeId PerModuleSema::inferStructLiteral(frontend::ExprId id) {
    const auto &expr        = snapshot.expressions()[id.value - 1U];
    const TypeId struct_tid = type_table.lookupNamed(expr.text);
    if (!struct_tid) {
        report(expr.span, "unknown struct type '" + expr.text + "'");
        return error_type;
    }
    const TypeId resolved = resolve(struct_tid);
    const auto *st        = type_table.struct_type(resolved);
    if (st == nullptr) {
        report(expr.span, "'" + expr.text + "' is not a struct type");
        return error_type;
    }
    // Type-check each field value against its declared type
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        const TypeId value_type = inferExpr(expr.operands[i]);
        int decl_idx            = -1;
        if (i < expr.field_names.size())
            decl_idx = type_table.fieldIndex(resolved, expr.field_names[i]);
        if (decl_idx >= 0) {
            const TypeId decl_type = st->fields[static_cast<size_t>(decl_idx)];
            if (!coerceValue(expr.operands[i], decl_type, value_type))
                reportCoercionFailure(expr.span, decl_type, value_type,
                                      "struct literal field type mismatch for '" +
                                          expr.field_names[i] + "'");
        }
    }
    return TypeId{resolved.intern_seq};
}

bool PerModuleSema::coercesTo(TypeId target, TypeId source) const noexcept {
    bool result = false;
    if (sameType(target, source)) {
        result = true;
    } else {
        const TypeId resolved_target = resolve(target);
        if (type_table.kindOf(resolved_target) == TypeKind::Optional) {
            if (resolve(source) == null_type) {
                result = true;
            } else if (const auto *opt = type_table.optional(resolved_target)) {
                result = sameType(opt->inner, source);
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
    if (ka == TypeKind::Unknown || kb == TypeKind::Unknown)
        return true;
    if (ka != kb)
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
    if (ka == TypeKind::Void || ka == TypeKind::Never || ka == TypeKind::Bool ||
        ka == TypeKind::Char || ka == TypeKind::String) {
        return true;
    }
    if (ka == TypeKind::Pointer) {
        const auto *pa = type_table.pointer(resolved_a);
        const auto *pb = type_table.pointer(resolved_b);
        return pa != nullptr && pb != nullptr && sameType(pa->pointee, pb->pointee);
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
    if (ka == TypeKind::Function) {
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
    return false;
}

TypeId PerModuleSema::resolve(TypeId t) const noexcept {
    t = type_table.canonical(t);
    while (const auto *alias = type_table.alias(t)) {
        t = type_table.canonical(alias->target);
    }
    return t;
}

TypeId PerModuleSema::concreteBase(TypeId t) const noexcept {
    return t;
}

TypeId PerModuleSema::typeOfLocalByName(frontend::ScopeId scope, std::string_view name) {
    if (!scope)
        return kInvalidTypeId;
    if (scope.value > snapshot.scopes().size())
        return kInvalidTypeId;
    const auto *resolved = findResolvedBinding(name, scope);
    if (resolved) {
        if (resolved->local)
            return typeOfLocal(resolved->local);
        if (resolved->declaration)
            return typeOfDecl(resolved->declaration);
    }
    return kInvalidTypeId;
}

TypeId PerModuleSema::typeOfResolvedName(frontend::ExprId id) {
    const auto *resolved = findResolvedExpr(id);
    if (!resolved)
        return kInvalidTypeId;
    if (resolved->foreignFunction != nullptr) {
        auto &parameters = type_table.makeTypeStorage();
        for (const auto &parameter : resolved->foreignFunction->parameters)
            parameters.push(lowerForeignType(parameter));
        return type_table.internFunction(parameters,
                                         lowerForeignType(resolved->foreignFunction->result));
    }
    if (resolved->declaration) {
        if (!resolved->target.module.empty() && resolved->target.module != module)
            return typeOfDeclInModule(resolved->target.module, resolved->declaration);
        return typeOfDecl(resolved->declaration);
    }
    if (resolved->local) {
        if (!resolved->target.module.empty() && resolved->target.module != module)
            return typeOfDeclInModule(resolved->target.module,
                                      frontend::DeclId{resolved->target.localSymbol.value});
        return typeOfLocal(resolved->local);
    }
    if (!resolved->target.module.empty() && resolved->target.localSymbol) {
        if (resolved->target.module == module)
            return typeOfDecl(frontend::DeclId{resolved->target.localSymbol.value});
        return typeOfDeclInModule(resolved->target.module,
                                  frontend::DeclId{resolved->target.localSymbol.value});
    }
    return kInvalidTypeId;
}

TypeId PerModuleSema::typeOfDeclInModule(session::ModuleKey target_module,
                                         frontend::DeclId id) const noexcept {
    if (!owner)
        return kInvalidTypeId;
    const auto *target = owner->findModuleSema(target_module);
    if (!target)
        return kInvalidTypeId;
    return target->typeOfDecl(id);
}

const session::ResolvedName *PerModuleSema::findResolvedExpr(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return nullptr;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    for (const auto &r : resolution.expressions) {
        if (r.name.empty())
            continue;
        if (expr.span.start == r.span.start && expr.span.end == r.span.end)
            return &r;
    }
    return nullptr;
}

const session::ResolvedName *
PerModuleSema::findResolvedBinding(std::string_view name, frontend::ScopeId scope) const noexcept {
    return session::lookupBinding(resolution, name, scope, snapshot.scopes());
}

std::string_view PerModuleSema::sourceText(frontend::TextSpan span) const noexcept {
    if (span.end <= span.start || span.end > snapshot.source().size())
        return {};
    return std::string_view(snapshot.source()).substr(span.start, span.size());
}

memory::Span PerModuleSema::toMemorySpan(frontend::TextSpan span) const noexcept {
    return memory::Span{0, span.start, span.end};
}

SemaPipeline::SemaPipeline(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                           const session::CompilationSnapshot &snapshot)
    : arena_(arena), diags_(diags), snapshot_(snapshot), type_table_(arena), typed_maps_(),
      modules_(arena), has_errors_(false) {}

bool SemaPipeline::run() {
    for (const auto &artifact_ptr : snapshot_.modules()) {
        const auto &artifact   = *artifact_ptr;
        const auto *resolution = snapshot_.findResolution(artifact.key);
        if (!resolution || !artifact.frontend)
            continue;

        auto *typed_map = arena_.make<TypedMap>(arena_);
        typed_maps_.insert(artifact.key, typed_map);

        auto *sema = arena_.make<PerModuleSema>(artifact.key, *artifact.frontend, *resolution,
                                                type_table_, *typed_map, arena_, this);
        modules_.push(sema);
        if (!sema->prepareTypes())
            has_errors_ = true;
    }
    for (auto *sema : modules_) {
        if (!sema->checkExpressions())
            has_errors_ = true;
    }

    for (const auto *sema : modules_) {
        for (const auto &d : sema->diagnostics) {
            diags_.report(d.severity, d.code, d.message, sema->toMemorySpan(d.primary_span));
        }
    }

    return !has_errors_;
}

bool SemaPipeline::hasErrors() const noexcept {
    return has_errors_;
}

PerModuleSema *SemaPipeline::findModuleSema(session::ModuleKey module) const noexcept {
    for (const auto *sema : modules_) {
        if (sema->module == module)
            return const_cast<PerModuleSema *>(sema);
    }
    return nullptr;
}

const TypedMap *SemaPipeline::findTypedMap(session::ModuleKey module) const noexcept {
    auto *value = typed_maps_.get(module);
    if (value == nullptr)
        return nullptr;
    return *value;
}

TypedMap &SemaPipeline::typedMap(session::ModuleKey module) noexcept {
    auto *value = typed_maps_.get(module);
    if (value && *value)
        return **value;
    auto *tm = arena_.make<TypedMap>(arena_);
    typed_maps_.insert(module, tm);
    return *tm;
}

} // namespace zith::sema::modern
