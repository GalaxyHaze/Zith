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

/// Parses an enum variant's explicit `= <int literal>` discriminant. Accepts a plain
/// literal or a unary-minus literal (`Red = -1`). Returns false for anything else.
bool explicitDiscriminant(const frontend::FrontendSnapshot &snapshot, frontend::ExprId id,
                          std::int64_t &value) {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "-" && !expr.operands.empty()) {
        if (expr.operands[0].value > snapshot.expressions().size())
            return false;
        const auto &operand = snapshot.expressions()[expr.operands[0].value - 1U];
        if (operand.kind != frontend::ExprKind::Literal || !looksInteger(operand.text))
            return false;
        value = -std::strtoll(operand.text.c_str(), nullptr, 10);
        return true;
    }
    if (expr.kind != frontend::ExprKind::Literal || !looksInteger(expr.text))
        return false;
    value = std::strtoll(expr.text.c_str(), nullptr, 10);
    return true;
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

bool isShiftOp(std::string_view text) {
    return text == "<<" || text == ">>";
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
    checkStructFieldDefaults();
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
    // Every primitive spelling has to be in the registry: `lowerTypeExpr` now rejects unknown type
    // names instead of inventing a permissive `Unknown` type that compares equal to everything.
    error_type = type_table.internName("error", TypeKind::Error);
    null_type  = type_table.internName("null", TypeKind::Never);
    void_type  = registerPrimitive("void", TypeKind::Void, 0, false);
    bool_type  = registerPrimitive("bool", TypeKind::Bool, 0, false);
    char_type  = registerPrimitive("char", TypeKind::Char, 0, false);

    struct IntSpelling {
        std::string_view name;
        uint8_t bits;
        bool isSigned;
    };
    static constexpr IntSpelling kIntegers[] = {
        {"i8", 8, true},     {"i16", 16, true},   {"i32", 32, true},    {"i64", 64, true},
        {"i128", 128, true}, {"isize", 64, true}, {"u8", 8, false},     {"u16", 16, false},
        {"u32", 32, false},  {"u64", 64, false},  {"u128", 128, false}, {"usize", 64, false},
    };
    for (const auto &spelling : kIntegers) {
        const TypeId id =
            registerPrimitive(spelling.name, TypeKind::Integer, spelling.bits, spelling.isSigned);
        if (spelling.name == "i32")
            i32_type = id;
        else if (spelling.name == "i64")
            i64_type = id;
    }
    f32_type = registerPrimitive("f32", TypeKind::Float, 32, true);
    f64_type = registerPrimitive("f64", TypeKind::Float, 64, true);
}

TypeId PerModuleSema::registerPrimitive(std::string_view name, TypeKind kind, uint8_t bits,
                                        bool is_signed) {
    // The TypeTable is shared by every module in the snapshot, so reuse an existing registration.
    if (const TypeId existing = type_table.lookupNamed(name))
        return existing;
    TypeId id;
    switch (kind) {
    case TypeKind::Integer:
        id = type_table.internInteger({bits, is_signed});
        break;
    case TypeKind::Float:
        id = type_table.internFloat({bits});
        break;
    default:
        id = type_table.internName(name, kind);
        break;
    }
    type_table.registerNamed(name, id);
    return id;
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
        currentDeclId_ = decl.id.value;
        if (!decl.genericParams.empty()) {
            std::vector<GenericBinding> bindings;
            bindings.reserve(decl.genericParams.size());
            for (size_t i = 0; i < decl.genericParams.size(); ++i) {
                TypeId param_type =
                    type_table.internGenericParam(decl.id.value, static_cast<uint32_t>(i));
                bindings.push_back(GenericBinding{decl.genericParams[i].name, param_type});
            }
            genericParams_[decl.id.value] = std::move(bindings);
        }
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
            // Underlying type defaults to i32 when the declaration writes no `: T`.
            TypeId underlying = decl.declaredType ? lowerTypeExpr(decl.declaredType) : i32_type;
            if (!underlying)
                underlying = i32_type;
            if (type_table.kindOf(resolve(underlying)) != TypeKind::Integer) {
                report(decl.span, "enum underlying type must be an integer type",
                       diagnostics::err::TypeMismatch);
                underlying = i32_type;
            }
            auto &variant_names = type_table.makeStringStorage();
            auto &discriminants = type_table.makeDiscStorage();
            int64_t next_value  = 0;
            for (const auto &variant : decl.parameters) {
                for (const auto &existing : variant_names) {
                    if (existing == variant.name) {
                        report(variant.span, "duplicate enum variant '" + variant.name + "'",
                               diagnostics::err::DuplicateDecl);
                        break;
                    }
                }
                if (variant.defaultValue) {
                    std::int64_t disc = 0;
                    if (explicitDiscriminant(snapshot, variant.defaultValue, disc))
                        next_value = disc;
                    else
                        report(variant.span, "enum variant discriminant must be an integer literal",
                               diagnostics::err::TypeMismatch);
                }
                // Store name in a stable arena allocation.
                char *buf = static_cast<char *>(arena.alloc(variant.name.size(), 1));
                std::memcpy(buf, variant.name.data(), variant.name.size());
                variant_names.push(std::string_view(buf, variant.name.size()));
                discriminants.push(next_value++);
            }
            TypeId et = type_table.internEnum(decl.name, underlying, variant_names, discriminants);
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
        currentDeclId_ = decl.id.value;
        if (decl.kind == frontend::DeclKind::Function) {
            TypeId fn_type     = typeOfDecl(decl.id);
            const auto *fn     = type_table.function(fn_type);
            currentReturnType_ = fn ? fn->result : kInvalidTypeId;
        } else {
            currentReturnType_ = kInvalidTypeId;
        }
        if (decl.body) {
            markers_.clear();
            collectMarkers(decl.body);
            (void)inferExpr(decl.body);
            markers_.clear();
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
    currentDeclId_ = 0;
}

void PerModuleSema::collectMarkers(frontend::ExprId id) {
    if (!id || id.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto &stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (stmt.kind == frontend::StmtKind::Marker && !stmt.label.empty())
                markers_.insert(stmt.label, uint8_t{1});
            if (stmt.expression)
                collectMarkers(stmt.expression);
        }
    } else {
        for (const auto operand : expr.operands)
            collectMarkers(operand);
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

TypeId PerModuleSema::lowerTypeExpr(frontend::TypeExprId id) {
    if (!id || id.value > snapshot.typeExpressions().size())
        return kInvalidTypeId;
    const auto &type = snapshot.typeExpressions()[id.value - 1U];
    switch (type.kind) {
    case frontend::TypeExprKind::Name: {
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
        // Unknown type names are an error: inventing a placeholder here used to make every
        // misspelled or unregistered type silently compatible with anything.
        if (const TypeId named = type_table.lookupNamed(type.name))
            return named;
        report(type.span, "unknown type '" + type.name + "'", diagnostics::err::UndefinedIdent);
        return error_type;
    }
    case frontend::TypeExprKind::Pointer:
    case frontend::TypeExprKind::Optional: {
        // `*void` / `?*void` are rejected; `raw opaque` remains the spelling for C interop.
        const TypeId inner =
            type.arguments.empty() ? kInvalidTypeId : lowerTypeExpr(type.arguments[0]);
        if (inner && resolve(inner) == void_type) {
            report(type.span, "pointer to 'void' is not allowed; use 'raw opaque' for C interop",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        return type.kind == frontend::TypeExprKind::Pointer ? type_table.internPointer(inner)
                                                            : type_table.internOptional(inner);
    }
    case frontend::TypeExprKind::Array:
        return type_table.internArray(type.arguments.empty() ? kInvalidTypeId
                                                             : lowerTypeExpr(type.arguments[0]),
                                      type.arrayLength);
    case frontend::TypeExprKind::Slice:
        return type_table.internSlice(type.arguments.empty() ? kInvalidTypeId
                                                             : lowerTypeExpr(type.arguments[0]));
    case frontend::TypeExprKind::Function: {
        auto &params = type_table.makeTypeStorage();
        for (size_t i = 0; i + 1 < type.arguments.size(); ++i)
            params.push(lowerTypeExpr(type.arguments[i]));
        const TypeId result =
            type.arguments.empty() ? kInvalidTypeId : lowerTypeExpr(type.arguments.back());
        return type_table.internFunction(params, result);
    }
    case frontend::TypeExprKind::Error:
        return kInvalidTypeId;
    }
    return kInvalidTypeId;
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
    case frontend::ExprKind::For:
        result = inferFor(id);
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
    case frontend::ExprKind::ArrayLiteral:
        result = inferArrayLiteral(id);
        break;
    case frontend::ExprKind::Cast:
        result = inferCast(id);
        break;
    case frontend::ExprKind::IsNull:
        result = inferIsNull(id);
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
    const auto *resolved = findResolvedExpr(id);
    if (resolved) {
        if (const TypeId resolved_type = typeOfResolvedName(id))
            return resolved_type;
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
    if (expr.text == "not" || expr.text == "!") {
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
    } else if (expr.text == "&") {
        // Address-of: produce pointer to operand type
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
    if (isComparisonOp(expr.text)) {
        if (!sameType(left, right))
            report(expr.span, "comparison between incompatible types",
                   diagnostics::err::TypeMismatch);
        result = bool_type;
    } else if (isShiftOp(expr.text)) {
        // Both operands must be integers (after the numeric-literal adaptation above) and
        // share the same type, matching LLVM's same-type CreateShl/CreateAShr requirement.
        const bool left_integer  = type_table.integer(resolve(left)) != nullptr;
        const bool right_integer = type_table.integer(resolve(right)) != nullptr;
        if (!left_integer || !right_integer) {
            report(expr.span, "shift operator expects integer operands",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else if (!sameType(left, right)) {
            report(expr.span, "shift operands have incompatible types",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else {
            result = left;
        }
    } else if (isArithmeticOp(expr.text)) {
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

TypeId PerModuleSema::inferCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    TypeId callee_type = inferExpr(expr.operands[0]);
    const auto *fn     = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function", diagnostics::err::NoMatchingFn);
        return error_type;
    }
    // A generic declaration has no instantiated type: calling it is a semantic error
    // until monomorphization lands. Report the same message the comptime solver uses.
    if (!expr.genericArgs.empty() || typeContainsGeneric(fn)) {
        report(expr.span, "generic parameter T has no concrete type",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    size_t arg_count = expr.operands.size() - 1;
    if (arg_count != fn->params.size()) {
        report(expr.span, "function call arity mismatch", diagnostics::err::NoMatchingFn);
        return fn->result;
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
        TypeId arg_type = inferExpr(expr.operands[i + 1]);
        if (!coerceValue(expr.operands[i + 1], fn->params[i], arg_type))
            reportCoercionFailure(expr.span, fn->params[i], arg_type,
                                  "function call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    return fn->result;
}

bool PerModuleSema::typeContainsGeneric(const FunctionType *fn) const noexcept {
    for (const auto param : fn->params)
        if (type_table.kindOf(param) == TypeKind::GenericParam)
            return true;
    return type_table.kindOf(fn->result) == TypeKind::GenericParam;
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
                report(stmt.span, "null requires an optional type annotation",
                       diagnostics::err::TypeMismatch);
                init_type = error_type;
            }
            setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Return) {
            checkReturnStatement(stmt);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Marker) {
            // Markers are labels for `jump`; their body is a regular block.
            if (stmt.expression)
                (void)inferExpr(stmt.expression);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Jump) {
            if (!markers_.contains(stmt.label)) {
                report(stmt.span, "jump to undefined marker '" + stmt.label + "'",
                       diagnostics::err::UndefinedIdent);
            }
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
        report(expr.span, "if condition must be boolean", diagnostics::err::TypeMismatch);
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
            report(expr.span, "loop condition must be boolean", diagnostics::err::TypeMismatch);
    }
    // The body must be inferred too, otherwise locals declared inside the loop never get a type.
    if (expr.operands.size() >= 2U)
        (void)inferExpr(expr.operands[1]);
    return void_type;
}

TypeId PerModuleSema::inferFor(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    // operands: [cond, body, step].
    if (!expr.operands.empty()) {
        TypeId cond = inferExpr(expr.operands[0]);
        if (!sameType(cond, bool_type))
            report(expr.span, "loop condition must be boolean", diagnostics::err::TypeMismatch);
    }
    if (expr.operands.size() >= 2U && expr.operands[1])
        (void)inferExpr(expr.operands[1]);
    if (expr.operands.size() >= 3U && expr.operands[2])
        (void)inferExpr(expr.operands[2]);
    return void_type;
}

TypeId PerModuleSema::inferCast(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
    // `as` casts through an unregistered type name are a barrier: report a single
    // UnsupportedSyntax instead of letting the unknown type cascade into 2001+3003.
    const auto &target_type = snapshot.typeExpressions()[expr.cast_type.value - 1U];
    if (target_type.kind == frontend::TypeExprKind::Name &&
        !type_table.lookupNamed(target_type.name)) {
        report(expr.span, "'as' casts to unknown types are not supported in this version",
               diagnostics::err::UnsupportedSyntax);
        return error_type;
    }
    const TypeId source = inferExpr(expr.operands[0]);
    const TypeId target = lowerTypeExpr(expr.cast_type);
    TypeId result       = target;
    if (!target) {
        report(expr.span, "unknown target type in 'as' conversion", diagnostics::err::TypeMismatch);
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
        report(expr.span, "'is null' requires an optional operand ('?T')",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    return bool_type;
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
        } else if (cond_type != bool_type && cond_type != error_type) {
            // A non-boolean condition is an equality pattern: `(0)` means `subject == 0`.
            if (!sameType(subject, cond_type) && !adaptNumericLiteral(condition, subject)) {
                report(expr.span,
                       "when case condition must be a boolean expression or match the subject "
                       "type",
                       diagnostics::err::TypeMismatch);
            }
        }
        const TypeId case_type = inferExpr(expr.operands[i + 1U]);
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
    const auto &expr    = snapshot.expressions()[id.value - 1U];
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
                                          std::string_view context, uint32_t fallback_code) {
    if (resolve(source) == null_type) {
        report(span, "cannot assign 'null' to a non-optional pointer; use '?*T'",
               diagnostics::err::TypeMismatch);
        return;
    }
    const TypeKind from = type_table.kindOf(resolve(source));
    const TypeKind to   = type_table.kindOf(resolve(target));
    if (classifyCast(from, to) != CastKind::Invalid) {
        report(span, "implicit numeric conversion is not allowed; use 'as'",
               diagnostics::err::TypeMismatch);
        return;
    }
    report(span, std::string(context), fallback_code);
}

void PerModuleSema::checkReturnStatement(const frontend::Statement &stmt) {
    if (!stmt.expression) {
        // `return;` in a function that promises a value is still a mismatch.
        if (currentReturnType_ && resolve(currentReturnType_) != void_type &&
            resolve(currentReturnType_) != error_type) {
            report(stmt.span, "return without a value in a function with a declared return type",
                   diagnostics::err::TypeMismatch);
        }
        return;
    }
    const TypeId value = inferExpr(stmt.expression);
    if (!currentReturnType_ || !value || value == error_type)
        return;
    if (!coerceValue(stmt.expression, currentReturnType_, value)) {
        reportCoercionFailure(stmt.span, currentReturnType_, value,
                              "return type does not match declared return type");
    }
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
            report(expr.span, "type is not indexable", diagnostics::err::TypeMismatch);
            break;
        }
    }
    return result;
}

TypeId PerModuleSema::inferField(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;
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
    // not a struct field access.
    if (const auto enum_type = enumVariantType(expr.operands[0], expr.text, expr.span))
        return *enum_type;
    TypeId object_type = inferExpr(expr.operands[0]);
    TypeId resolved    = resolve(object_type);
    const auto *st     = type_table.struct_type(resolved);
    if (st == nullptr) {
        report(expr.span, "field access on non-struct type", diagnostics::err::TypeMismatch);
        return error_type;
    }
    int idx = type_table.fieldIndex(resolved, expr.text);
    if (idx < 0) {
        report(expr.span, "unknown field '" + expr.text + "'", diagnostics::err::NoMember);
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
        report(expr.span, "'->' on a pointer to non-struct type", diagnostics::err::TypeMismatch);
        return error_type;
    }
    int idx = type_table.fieldIndex(struct_type, expr.text);
    if (idx < 0) {
        report(expr.span, "unknown field '" + expr.text + "'", diagnostics::err::NoMember);
        return error_type;
    }
    return st->fields[static_cast<size_t>(idx)];
}

TypeId PerModuleSema::inferStructLiteral(frontend::ExprId id) {
    const auto &expr        = snapshot.expressions()[id.value - 1U];
    const TypeId struct_tid = type_table.lookupNamed(expr.text);
    if (!struct_tid) {
        report(expr.span, "unknown struct type '" + expr.text + "'",
               diagnostics::err::UndefinedIdent);
        return error_type;
    }
    const TypeId resolved = resolve(struct_tid);
    const auto *st        = type_table.struct_type(resolved);
    if (st == nullptr) {
        report(expr.span, "'" + expr.text + "' is not a struct type");
        return error_type;
    }
    const size_t field_count = st->fields.size();
    const bool named         = !expr.field_names.empty();
    const auto fieldName     = [&](const int index) -> std::string {
        if (index >= 0 && static_cast<size_t>(index) < st->field_names.size())
            return std::string(st->field_names[static_cast<size_t>(index)]);
        return expr.text;
    };
    std::vector<bool> seen(field_count, false);
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        int decl_idx = -1;
        if (named) {
            decl_idx = type_table.fieldIndex(resolved, expr.field_names[i]);
            if (decl_idx < 0) {
                report(expr.span,
                       "unknown field '" + expr.field_names[i] + "' in struct '" + expr.text + "'",
                       diagnostics::err::NoMember);
                continue;
            }
        } else {
            decl_idx = static_cast<int>(i);
            if (i >= field_count) {
                report(expr.span, "too many fields in struct literal for '" + expr.text + "'",
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
        const TypeId decl_type              = st->fields[static_cast<size_t>(decl_idx)];
        const auto &operand                 = snapshot.expressions()[expr.operands[i].value - 1U];
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
        }
    }
    return TypeId{resolved.intern_seq};
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
            report(expr.span, "array literal element types do not match",
                   diagnostics::err::TypeMismatch);
            return error_type;
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

void PerModuleSema::checkStructFieldDefaults() {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Struct)
            continue;
        const TypeId struct_type = type_table.lookupNamed(decl.name);
        if (!struct_type)
            continue;
        const auto *st = type_table.struct_type(resolve(struct_type));
        if (st == nullptr)
            continue;
        for (size_t index = 0; index < decl.parameters.size() && index < st->fields.size();
             ++index) {
            const auto default_id = decl.parameters[index].defaultValue;
            if (!default_id)
                continue;
            const TypeId value_type = inferExpr(default_id);
            if (!coerceValue(default_id, st->fields[index], value_type)) {
                reportCoercionFailure(decl.parameters[index].span, st->fields[index], value_type,
                                      "struct field default type mismatch for '" +
                                          decl.parameters[index].name + "'");
            }
        }
    }
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
    // `error` suppresses cascading diagnostics; `Unknown` (generics / type vars) does not.
    if (resolved_a == error_type || resolved_b == error_type)
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
