#include "sema/sema-modern.hpp"

#include "diagnostics/error-codes.hpp"
#include "sema/op-mapping.hpp"
#include "support/int-literal.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>

namespace zith::sema::modern {

namespace {

/// Accepts every integer literal the lexer produces, including the explicit radix
/// forms `0x`/`0c`/`0b`. Keeping this in sync with the lexer matters: a literal that
/// is lexed but not recognised here infers as `error` and silently produces no value.
bool looksInteger(std::string_view text) {
    return support::looksIntegerLiteral(text);
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

bool looksChar(std::string_view text) {
    return text.size() >= 3 && text.front() == '\'' && text.back() == '\'';
}

/// The single decision point for `as` conversions. User-defined casts will become a new
/// branch here rather than a new call site.
enum class CastKind : uint8_t {
    Invalid,
    Identity,
    IntToInt,
    IntToFloat,
    FloatToInt,
    FloatToFloat,
    PtrToPtr
};

[[nodiscard]] CastKind classifyCast(TypeKind from, TypeKind to) {
    const bool from_integer = from == TypeKind::Integer || from == TypeKind::Char;
    const bool to_integer   = to == TypeKind::Integer || to == TypeKind::Char;
    const bool from_enum    = from == TypeKind::Enum;
    if (from == to && from == TypeKind::Float)
        return CastKind::FloatToFloat;
    if ((from_integer || from_enum) && to_integer)
        return CastKind::IntToInt;
    if (from_integer && to == TypeKind::Float)
        return CastKind::IntToFloat;
    if (from == TypeKind::Float && to_integer)
        return CastKind::FloatToInt;
    return CastKind::Invalid;
}

} // namespace

PerModuleSema::PerModuleSema(session::ModuleKey mod, const frontend::FrontendSnapshot &snap,
                             const session::ModuleResolution &res, TypeTable &tt, TypedMap &tm,
                             memory::Arena &a, memory::FileId file_id, SemaPipeline *owner_)
    : module(std::move(mod)), fileId(file_id), snapshot(snap), resolution(res), type_table(tt),
      typed_map(tm), arena(a), diagnostics(a), owner(owner_), error_type(kInvalidTypeId),
      invalid_type(kInvalidTypeId), void_type(kInvalidTypeId), bool_type(kInvalidTypeId),
      char_type(kInvalidTypeId), i32_type(kInvalidTypeId), i64_type(kInvalidTypeId),
      f32_type(kInvalidTypeId), f64_type(kInvalidTypeId) {}

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
    checkConstFieldAssignments();
    checkZithDeclarations();
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
    error_type   = type_table.internName("error", TypeKind::Error);
    invalid_type = type_table.internInvalid();
    null_type    = type_table.internName("null", TypeKind::Never);
    void_type    = registerPrimitive("void", TypeKind::Void, 0, false);
    bool_type    = registerPrimitive("bool", TypeKind::Bool, 0, false);
    char_type    = registerPrimitive("char", TypeKind::Char, 0, false);

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
        case frontend::DeclKind::Marker:
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
        currentDeclId_       = decl.id.value;
        currentFunctionKind_ = decl.kind == frontend::DeclKind::Function
                                   ? decl.functionKind
                                   : frontend::FunctionKind::Standard;
        if (!decl.genericParams.empty()) {
            std::vector<GenericBinding> bindings;
            bindings.reserve(decl.genericParams.size());
            for (size_t i = 0; i < decl.genericParams.size(); ++i) {
                TypeId param_type =
                    type_table.internGenericParam(decl.id.value, static_cast<uint32_t>(i));
                bindings.push_back(GenericBinding{decl.genericParams[i].name, param_type});
            }
            // Methods inside `implement Owner<T>` inherit the owner's generic
            // parameter names. Reuse the owner's GenericParam TypeId so fields of
            // `Owner<T>` and the method's own `T` signatures unify before the
            // monomorphization pass substitutes the concrete arguments.
            if (!decl.ownerName.empty()) {
                for (const auto &owner_decl : snapshot.declarations()) {
                    if (owner_decl.name != decl.ownerName || owner_decl.genericParams.empty())
                        continue;
                    if (owner_decl.kind != frontend::DeclKind::Struct &&
                        owner_decl.kind != frontend::DeclKind::TypeAlias &&
                        owner_decl.kind != frontend::DeclKind::Enum &&
                        owner_decl.kind != frontend::DeclKind::Union)
                        continue;
                    for (size_t index = 0; index < owner_decl.genericParams.size(); ++index) {
                        for (auto &binding : bindings) {
                            if (binding.name == owner_decl.genericParams[index].name)
                                binding.type = type_table.internGenericParam(
                                    owner_decl.id.value, static_cast<uint32_t>(index));
                        }
                    }
                    break;
                }
            }
            genericParams_[decl.id.value] = std::move(bindings);
        }
        switch (decl.kind) {
        case frontend::DeclKind::Marker: {
            for (const auto &param : decl.parameters) {
                TypeId ptype = lowerTypeExpr(param.type);
                if (!ptype)
                    ptype = error_type;
                setLocalType(param.id, ptype);
            }
            setDeclType(decl.id, void_type);
            break;
        }
        case frontend::DeclKind::Function: {
            auto &params_storage = type_table.makeTypeStorage();
            bool is_method       = !decl.ownerName.empty();
            TypeId owner_type    = kInvalidTypeId;
            if (is_method) {
                owner_type = type_table.lookupNamed(decl.ownerName);
                if (!owner_type) {
                    report(decl.span, "owner type '" + decl.ownerName + "' is not defined",
                           diagnostics::err::UndefinedIdent);
                }
            }
            for (size_t i = 0; i < decl.parameters.size(); ++i) {
                const auto &param = decl.parameters[i];
                TypeId ptype      = lowerTypeExpr(param.type);
                if (!ptype)
                    ptype = error_type;
                // A first parameter named 'self' with no explicit type in a
                // method gets the owner pointer type implicitly.
                if (is_method && i == 0 && param.name == "self" && !decl.parameters.front().type &&
                    owner_type) {
                    ptype = type_table.internPointer(owner_type);
                }
                setLocalType(param.id, ptype);
                params_storage.push(ptype);
            }
            // Methods without a declared receiver parameter are static in this
            // compiler. They keep the exact parameter list they were written
            // with, so `Type.foo()` resolves to a zero-parameter signature.
            // Only an actual `self` parameter receives the owner pointer type.
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
                vtype = decl.initializer ? inferExpr(decl.initializer) : error_type;
            setDeclType(decl.id, vtype);
            break;
        }
        case frontend::DeclKind::TypeAlias: {
            TypeId target = lowerTypeExpr(decl.declaredType);
            if (target) {
                TypeId alias = decl.isNominalType ? type_table.internNominal(decl.name, target)
                                                  : type_table.internAlias(target);
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
            std::function<bool(frontend::ExprId, std::int64_t &, unsigned)> evaluate =
                [&](frontend::ExprId id, std::int64_t &out, unsigned guard) -> bool {
                if (!id || id.value > snapshot.expressions().size() || guard > 64U)
                    return false;
                const auto &expr = snapshot.expressions()[id.value - 1U];
                switch (expr.kind) {
                case frontend::ExprKind::Literal:
                    return looksInteger(expr.text) &&
                           support::parseIntegerLiteral(expr.text, out) ==
                               support::IntLiteralStatus::Ok;
                case frontend::ExprKind::Unary: {
                    if (expr.operands.empty() || (expr.text != "-" && expr.text != "~"))
                        return false;
                    std::int64_t operand = 0;
                    if (!evaluate(expr.operands[0], operand, guard + 1U))
                        return false;
                    if (expr.text == "-" && operand == std::numeric_limits<std::int64_t>::min())
                        return false;
                    out = expr.text == "-" ? -operand : ~operand;
                    return true;
                }
                case frontend::ExprKind::Binary: {
                    if (expr.operands.size() < 2U)
                        return false;
                    std::int64_t left  = 0;
                    std::int64_t right = 0;
                    if (!evaluate(expr.operands[0], left, guard + 1U) ||
                        !evaluate(expr.operands[1], right, guard + 1U))
                        return false;
                    const bool division = expr.text == "/" || expr.text == "%";
                    const bool shift    = expr.text == "<<" || expr.text == ">>";
                    if (division &&
                        (right == 0 ||
                         (left == std::numeric_limits<std::int64_t>::min() && right == -1)))
                        return false;
                    if (shift && (right < 0 || right >= 64))
                        return false;
                    const __int128 wide = [&]() {
                        if (expr.text == "+")
                            return static_cast<__int128>(left) + right;
                        if (expr.text == "-")
                            return static_cast<__int128>(left) - right;
                        if (expr.text == "*")
                            return static_cast<__int128>(left) * right;
                        if (expr.text == "/") {
                            return static_cast<__int128>(left) / right;
                        }
                        if (expr.text == "%") {
                            return static_cast<__int128>(left) % right;
                        }
                        if (expr.text == "&.")
                            return static_cast<__int128>(static_cast<std::uint64_t>(left) &
                                                         static_cast<std::uint64_t>(right));
                        if (expr.text == "|.")
                            return static_cast<__int128>(static_cast<std::uint64_t>(left) |
                                                         static_cast<std::uint64_t>(right));
                        if (expr.text == "^.")
                            return static_cast<__int128>(static_cast<std::uint64_t>(left) ^
                                                         static_cast<std::uint64_t>(right));
                        if (expr.text == "<<") {
                            return static_cast<__int128>(static_cast<std::uint64_t>(left)
                                                         << static_cast<unsigned>(right));
                        }
                        if (expr.text == ">>") {
                            return static_cast<__int128>(left >> static_cast<unsigned>(right));
                        }
                        if (expr.text == "==")
                            return static_cast<__int128>(left == right);
                        if (expr.text == "!=")
                            return static_cast<__int128>(left != right);
                        if (expr.text == "<")
                            return static_cast<__int128>(left < right);
                        if (expr.text == "<=")
                            return static_cast<__int128>(left <= right);
                        if (expr.text == ">")
                            return static_cast<__int128>(left > right);
                        if (expr.text == ">=")
                            return static_cast<__int128>(left >= right);
                        return static_cast<__int128>(0);
                    }();
                    const std::int64_t min = std::numeric_limits<std::int64_t>::min();
                    const std::int64_t max = std::numeric_limits<std::int64_t>::max();
                    if (wide < min || wide > max)
                        return false;
                    out = static_cast<std::int64_t>(wide);
                    return expr.text == "+" || expr.text == "-" || expr.text == "*" ||
                           expr.text == "/" || expr.text == "%" || expr.text == "&." ||
                           expr.text == "|." || expr.text == "^." || expr.text == "<<" ||
                           expr.text == ">>" || expr.text == "==" || expr.text == "!=" ||
                           expr.text == "<" || expr.text == "<=" || expr.text == ">" ||
                           expr.text == ">=";
                }
                case frontend::ExprKind::Name: {
                    for (size_t i = 0; i < variant_names.size(); ++i) {
                        if (variant_names[i] == expr.text) {
                            out = discriminants[i];
                            return true;
                        }
                    }
                    const auto *resolved = findResolvedExpr(id);
                    if (resolved == nullptr ||
                        resolved->bindingKind != frontend::BindingKind::Const)
                        return false;
                    const TypeId const_type = typeOfResolvedName(id);
                    if (!const_type ||
                        type_table.integer(type_table.stripQualifiers(const_type)) == nullptr)
                        return false;
                    if (resolved->declaration &&
                        resolved->declaration.value <= snapshot.declarations().size()) {
                        const auto &const_decl =
                            snapshot.declarations()[resolved->declaration.value - 1U];
                        return evaluate(const_decl.initializer, out, guard + 1U);
                    }
                    for (const auto &statement : snapshot.statements()) {
                        if (statement.kind == frontend::StmtKind::Binding &&
                            statement.binding.id == resolved->local)
                            return evaluate(statement.binding.initializer, out, guard + 1U);
                    }
                    return false;
                }
                case frontend::ExprKind::Field: {
                    if (expr.operands.empty())
                        return false;
                    const auto *base = findResolvedExpr(expr.operands[0]);
                    if (base == nullptr || base->declaration != decl.id ||
                        base->declKind != frontend::DeclKind::Enum)
                        return false;
                    for (size_t i = 0; i < variant_names.size(); ++i) {
                        if (variant_names[i] == expr.text) {
                            out = discriminants[i];
                            return true;
                        }
                    }
                    return false;
                }
                default:
                    return false;
                }
            };
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
                    if (evaluate(variant.defaultValue, disc, 0U)) {
                        const auto *int_type =
                            type_table.integer(type_table.stripQualifiers(underlying));
                        const bool fits_signed = [&]() {
                            if (!int_type || !int_type->isSigned || int_type->bits >= 64)
                                return true;
                            const std::int64_t max = (std::int64_t{1} << (int_type->bits - 1U)) - 1;
                            const std::int64_t min = -max - 1;
                            return disc >= min && disc <= max;
                        }();
                        const bool fits_unsigned = [&]() {
                            if (!int_type || int_type->isSigned)
                                return true;
                            if (disc < 0)
                                return false;
                            if (int_type->bits >= 64)
                                return true;
                            const std::uint64_t max = (std::uint64_t{1} << int_type->bits) - 1U;
                            return static_cast<std::uint64_t>(disc) <= max;
                        }();
                        if (!fits_signed || !fits_unsigned) {
                            report(variant.span,
                                   "enum variant discriminant does not fit its underlying type '" +
                                       type_table.typeToString(underlying) + "'",
                                   diagnostics::err::TypeMismatch);
                        }
                        next_value = disc;
                    } else {
                        report(variant.span,
                               "enum variant discriminant must be a constant integer expression",
                               diagnostics::err::TypeMismatch);
                    }
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
            for (const auto &member_param : decl.parameters) {
                TypeId member = lowerTypeExpr(member_param.type);
                if (!member || member == error_type || member == invalid_type) {
                    report(member_param.span, "union member must be a concrete type",
                           diagnostics::err::TypeMismatch);
                    continue;
                }
                const TypeId resolved_member = resolve(member);
                if (type_table.kindOf(resolved_member) == TypeKind::Void) {
                    report(member_param.span, "union member cannot be 'void'",
                           diagnostics::err::TypeMismatch);
                    continue;
                }
                if (type_table.kindOf(resolved_member) == TypeKind::Unknown) {
                    report(member_param.span, "union member must not be an unknown type",
                           diagnostics::err::TypeMismatch);
                    continue;
                }
                members.push(member);
            }
            TypeId ut = type_table.internUnion(decl.name, members, !decl.isRawUnion);
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
    // Global markers can jump to markers declared later in the same module.
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Marker)
            global_markers_.insert({decl.name, &decl});
    }
    for (const auto &decl : snapshot.declarations()) {
        // A macro declaration is a template, not code: its body only becomes
        // real code once cloned into a call site.
        if (decl.kind == frontend::DeclKind::Macro)
            continue;
        currentDeclId_       = decl.id.value;
        currentFunctionKind_ = decl.kind == frontend::DeclKind::Function
                                   ? decl.functionKind
                                   : frontend::FunctionKind::Standard;
        if (decl.kind == frontend::DeclKind::Function) {
            TypeId fn_type     = typeOfDecl(decl.id);
            const auto *fn     = type_table.function(fn_type);
            currentReturnType_ = fn ? fn->result : kInvalidTypeId;
        } else {
            currentReturnType_ = kInvalidTypeId;
        }
        if (decl.body) {
            currentDeclId_ = decl.id.value;
            if (decl.kind == frontend::DeclKind::Function) {
                local_markers_.clear();
                collectMarkers(decl.body);
                inMarkerBody_ = false;
            } else if (decl.kind == frontend::DeclKind::Marker) {
                inMarkerBody_           = true;
                inGlobalMarker_         = true;
                currentStacklessMarker_ = true;
                currentFunctionKind_    = frontend::FunctionKind::Flow;
                currentReturnType_      = kInvalidTypeId;
            }
            (void)inferExpr(decl.body);
            inMarkerBody_           = false;
            inGlobalMarker_         = false;
            currentStacklessMarker_ = false;
            if (decl.kind == frontend::DeclKind::Function)
                local_markers_.clear();
        }
        if (decl.initializer) {
            (void)inferExpr(decl.initializer);
        }
    }
    // Also infer standalone expressions, skipping macro template bodies.
    for (const auto &expr : snapshot.expressions()) {
        if (snapshot.isMacroTemplateExpr(expr.id))
            continue;
        if (!typeOfExpr(expr.id))
            (void)inferExpr(expr.id);
    }
    currentDeclId_       = 0;
    currentFunctionKind_ = frontend::FunctionKind::Standard;
}

void PerModuleSema::collectMarkers(frontend::ExprId id) {
    if (!id || id.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    // A macro body is a template, but once it is expanded its nodes carry the
    // call-site ID and must be visible to marker collection just like any
    // regular expression tree.
    if (expr.kind == frontend::ExprKind::MacroCall && expr.expansion) {
        collectMarkers(expr.expansion);
        return;
    }
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto &stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (stmt.kind == frontend::StmtKind::Marker && !stmt.label.empty())
                local_markers_.insert({stmt.label, LocalMarker{&stmt, stmt.isStackful}});
            if (stmt.expression)
                collectMarkers(stmt.expression);
        }
    } else {
        for (const auto operand : expr.operands)
            collectMarkers(operand);
    }
}

TypeId PerModuleSema::markerParamType(const frontend::Parameter &param) {
    TypeId param_type = lowerTypeExpr(param.type);
    if (!param_type)
        param_type = error_type;
    setLocalType(param.id, param_type);
    return param_type;
}

void PerModuleSema::validateMarkerReference(frontend::TextSpan span, std::string_view name,
                                            const frontend::Statement &use) {
    const frontend::Declaration *global = nullptr;
    if (const auto found = global_markers_.find(std::string(name)); found != global_markers_.end())
        global = found->second;

    const frontend::Statement *declaration = nullptr;
    bool stackful                          = false;
    if (const auto found = local_markers_.find(std::string(name)); found != local_markers_.end()) {
        declaration = found->second.statement;
        stackful    = found->second.stackful;
    } else if (global != nullptr) {
        declaration = nullptr;
    }

    if (global == nullptr && declaration == nullptr) {
        report(span, "jump to undefined marker '" + std::string(name) + "'",
               diagnostics::err::UndefinedIdent);
        return;
    }

    const auto *params = declaration != nullptr ? &declaration->parameters : &global->parameters;
    if (use.arguments.size() != params->size()) {
        report(use.span,
               "marker '" + std::string(name) + "' expects " + std::to_string(params->size()) +
                   " argument(s), got " + std::to_string(use.arguments.size()),
               diagnostics::err::NoMatchingFn);
        return;
    }
    for (size_t index = 0; index < use.arguments.size(); ++index) {
        TypeId arg_type = inferExpr(use.arguments[index]);
        if (!arg_type)
            continue;
        TypeId param_type = declaration != nullptr ? markerParamType((*params)[index])
                                                   : markerParamType((*params)[index]);
        if (!coerceValue(use.arguments[index], param_type, arg_type))
            reportCoercionFailure(use.arguments[index].value <= snapshot.expressions().size()
                                      ? snapshot.expressions()[use.arguments[index].value - 1U].span
                                      : use.span,
                                  param_type, arg_type, "marker argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    (void)stackful;
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
    const TypeId bare{lowerBareTypeExpr(type)};
    // A memory qualifier wraps the type it annotates. `resolve()` sees through the
    // wrapper, so unification, coercion and casts are unchanged, while the
    // qualifier stays recoverable for the mutability checks.
    if (bare && (type.ownership != frontend::OwnershipKind::Default || type.isMut))
        return type_table.internQualified(bare, mapOwnership(type.ownership), type.isMut);
    return TypeId{bare};
}

TypeId PerModuleSema::lowerBareTypeExpr(const frontend::TypeExpression &type) {
    switch (type.kind) {
    case frontend::TypeExprKind::Name: {
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
    case frontend::TypeExprKind::Opaque:
        // `raw opaque` is the only accepted spelling of an untyped pointer; a literally
        // written `*void` is still rejected above, via TypeExprKind::Pointer.
        return type_table.internPointer(void_type);
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
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i)
            activeTemplateArgs_.push_back(
                GenericBinding{template_decl->genericParams[i].name, args[i]});
        for (const auto &param : template_decl->parameters) {
            TypeId ftype = lowerTypeExpr(param.type);
            fields.push(ftype ? ftype : error_type);
            char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
            std::memcpy(buf, param.name.data(), param.name.size());
            fld_names.push(std::string_view(buf, param.name.size()));
        }
        activeTemplateArgs_ = std::move(saved_active);
        TypeId st           = type_table.internStruct(concrete_name, fields, &fld_names);
        type_table.registerNamed(concrete_name, st);
        return st;
    }
    case frontend::DeclKind::TypeAlias: {
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i)
            activeTemplateArgs_.push_back(
                GenericBinding{template_decl->genericParams[i].name, args[i]});
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
    const std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
    activeTemplateArgs_.clear();
    for (size_t i = 0; i < template_decl.genericParams.size(); ++i)
        activeTemplateArgs_.push_back(GenericBinding{template_decl.genericParams[i].name, args[i]});
    for (const auto &param : template_decl.parameters) {
        const TypeId field_type = lowerTypeExpr(param.type);
        fields.push(field_type ? field_type : error_type);
        char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
        std::memcpy(buf, param.name.data(), param.name.size());
        field_names.push(std::string_view(buf, param.name.size()));
    }
    activeTemplateArgs_ = saved_active;

    const TypeId st = type_table.internStruct(concrete_name, fields, &field_names);
    type_table.registerNamed(concrete_name, st);
    return st;
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
    } else if (expr.text == "~") {
        if (type_table.integer(resolve(operand)) == nullptr) {
            report(expr.span, "unary '~' expects an integer operand",
                   diagnostics::err::TypeMismatch);
            result = error_type;
        } else {
            result = operand;
        }
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

const PerModuleSema::CallTarget *
PerModuleSema::resolvedCallTarget(frontend::ExprId callee) const noexcept {
    const auto found = call_targets_.find(callee.value);
    return found == call_targets_.end() ? nullptr : &found->second;
}

void PerModuleSema::setResolvedCallTarget(frontend::ExprId callee, session::ModuleKey target_module,
                                          frontend::DeclId decl) {
    if (!callee || !decl)
        return;
    call_targets_[callee.value] = CallTarget{std::move(target_module), decl};
}

TypeId PerModuleSema::typeOfResolvedBinding(const session::ResolvedName &binding) {
    if (binding.foreignFunction != nullptr) {
        auto &parameters = type_table.makeTypeStorage();
        for (const auto &parameter : binding.foreignFunction->parameters)
            parameters.push(lowerForeignType(parameter));
        return type_table.internFunction(parameters,
                                         lowerForeignType(binding.foreignFunction->result));
    }
    if (binding.declaration) {
        if (!binding.target.module.empty() && binding.target.module != module)
            return typeOfDeclInModule(binding.target.module, binding.declaration);
        return typeOfDecl(binding.declaration);
    }
    if (!binding.target.module.empty() && binding.target.localSymbol) {
        const frontend::DeclId decl{binding.target.localSymbol.value};
        if (binding.target.module == module)
            return typeOfDecl(decl);
        return typeOfDeclInModule(binding.target.module, decl);
    }
    return kInvalidTypeId;
}

bool PerModuleSema::literalAdaptsTo(frontend::ExprId value, TypeId target) const noexcept {
    if (!value || value.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[value.value - 1U];
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "-" && !expr.operands.empty())
        return literalAdaptsTo(expr.operands[0], target);
    if (expr.kind != frontend::ExprKind::Literal)
        return false;
    const TypeKind target_kind = type_table.kindOf(resolve(target));
    const bool integer_literal = looksInteger(expr.text);
    const bool float_literal   = looksFloat(expr.text);
    if (!integer_literal && !float_literal)
        return false;
    // Probing is deliberately stricter than `adaptNumericLiteral`: an integer
    // literal only fits an integer parameter and a float literal only a float
    // one.  Otherwise `add(1, 2)` would match both the i32 and the f64 overload
    // and every such call would be ambiguous.
    if (target_kind == TypeKind::Integer)
        return integer_literal;
    if (target_kind == TypeKind::Float)
        return float_literal;
    return false;
}

const PerModuleSema::OverloadCandidate *
PerModuleSema::selectOverload(const frontend::Expression &call,
                              const std::vector<OverloadCandidate> &candidates,
                              size_t implicit_args, bool &reported) {
    reported                  = false;
    const size_t written_args = call.operands.size() - 1U;
    std::vector<const OverloadCandidate *> viable;
    std::vector<const OverloadCandidate *> exact_matches;
    bool widened_pointer = false;
    for (const auto &candidate : candidates) {
        if (candidate.fn == nullptr)
            continue;
        if (candidate.fn->params.size() != written_args + implicit_args)
            continue;
        bool fits       = true;
        bool exact      = true;
        bool widens_ptr = false;
        for (size_t index = 0; index < written_args && fits; ++index) {
            const frontend::ExprId arg = call.operands[index + 1U];
            const TypeId arg_type      = inferExpr(arg);
            const TypeId param_type    = candidate.fn->params[index + implicit_args];
            // Probe without mutating the recorded type: a rejected candidate must
            // not leave a literal retyped for a signature that was not chosen.
            fits            = coercesTo(param_type, arg_type) || literalAdaptsTo(arg, param_type);
            const bool same = sameType(param_type, arg_type);
            exact           = exact && same;
            widens_ptr      = widens_ptr || (!same && isVoidPointer(param_type) &&
                                        static_cast<bool>(pointerBase(arg_type)));
        }
        if (fits) {
            viable.push_back(&candidate);
            if (exact)
                exact_matches.push_back(&candidate);
            if (widens_ptr)
                widened_pointer = true;
        }
    }
    // Any pointer coerces to a `void*` parameter, so `f(*i32)` and `f(raw opaque)` are both
    // viable for a `*i32` argument. Only that widening is ranked away by preferring the
    // exact signature; every other overload tie (including numeric literals) stays ambiguous.
    if (widened_pointer && !exact_matches.empty())
        viable = std::move(exact_matches);
    if (viable.size() == 1U)
        return viable.front();

    if (viable.empty()) {
        report(call.span, "no overload of this function accepts the given arguments",
               diagnostics::err::NoMatchingFn);
    } else {
        report(call.span, "call is ambiguous between several overloads",
               diagnostics::err::AmbiguousCall);
    }
    for (const auto &candidate : candidates)
        reportNote(candidate.span, "candidate declared here");
    reported = true;
    return nullptr;
}

TypeId PerModuleSema::inferCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;

    // If the callee is a field access, try to resolve it as a method call
    // first: `obj.method(args)` rewrites to a direct call with an implicit
    // `&obj` (or `obj` when already a pointer) as the first argument.
    const auto &callee = snapshot.expressions()[expr.operands[0].value - 1U];
    if ((callee.kind == frontend::ExprKind::Field || callee.kind == frontend::ExprKind::Arrow) &&
        !callee.operands.empty()) {
        if (const TypeId mt = inferMethodCall(expr, callee))
            return mt;
    }

    // An overload set: several `fn` declarations share the callee's name in the
    // nearest scope that declares it.  Pick by arity, then by argument fit.
    if (callee.kind == frontend::ExprKind::Name) {
        const auto bindings =
            session::lookupOverloads(resolution, callee.text, callee.scope, snapshot.scopes());
        if (bindings.size() > 1U) {
            std::vector<OverloadCandidate> candidates;
            candidates.reserve(bindings.size());
            for (const auto *binding : bindings) {
                OverloadCandidate candidate;
                candidate.binding = binding;
                candidate.type    = typeOfResolvedBinding(*binding);
                candidate.fn      = type_table.function(candidate.type);
                candidate.span    = binding->span;
                if (candidate.fn != nullptr && !typeContainsGeneric(candidate.fn))
                    candidates.push_back(candidate);
            }
            if (candidates.size() > 1U) {
                bool reported = false;
                if (const auto *chosen = selectOverload(expr, candidates, 0U, reported)) {
                    for (size_t index = 0; index < chosen->fn->params.size(); ++index) {
                        const TypeId arg_type = inferExpr(expr.operands[index + 1U]);
                        if (!coerceValue(expr.operands[index + 1U], chosen->fn->params[index],
                                         arg_type)) {
                            reportCoercionFailure(expr.span, chosen->fn->params[index], arg_type,
                                                  "function call argument type mismatch",
                                                  diagnostics::err::NoMatchingFn);
                        }
                    }
                    setExprType(callee.id, chosen->type);
                    const auto &binding   = *chosen->binding;
                    frontend::DeclId decl = binding.declaration;
                    session::ModuleKey target =
                        binding.target.module.empty() ? module : binding.target.module;
                    if (!decl && binding.target.localSymbol)
                        decl = frontend::DeclId{binding.target.localSymbol.value};
                    setResolvedCallTarget(callee.id, target, decl);
                    return chosen->fn->result;
                }
                if (reported)
                    return error_type;
            }
        }
    }

    const auto *resolved_callee = findResolvedExpr(callee.id);
    const bool is_variadic      = resolved_callee != nullptr && bindingIsVariadic(*resolved_callee);
    TypeId callee_type          = inferExpr(expr.operands[0]);
    const auto *fn              = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function", diagnostics::err::NoMatchingFn);
        return error_type;
    }
    size_t arg_count             = expr.operands.size() - 1;
    const size_t fixed_arg_count = is_variadic ? fn->params.size() : 0;
    if (is_variadic) {
        if (arg_count < fixed_arg_count) {
            report(expr.span, "variadic function call has too few arguments",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
    } else if (arg_count != fn->params.size()) {
        report(expr.span, "function call arity mismatch", diagnostics::err::NoMatchingFn);
        return fn->result;
    }

    if (!expr.genericArgs.empty() || typeContainsGeneric(fn)) {
        if (instantiations == nullptr) {
            report(expr.span, "generic function calls require the instantiation pass",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        }

        frontend::DeclId decl_id{};
        if (resolved_callee != nullptr) {
            if (resolved_callee->declaration)
                decl_id = resolved_callee->declaration;
            else if (resolved_callee->target.localSymbol)
                decl_id = frontend::DeclId{resolved_callee->target.localSymbol.value};
        }
        const frontend::Declaration *generic_decl = nullptr;
        if (decl_id && decl_id.value <= snapshot.declarations().size())
            generic_decl = &snapshot.declarations()[decl_id.value - 1U];
        session::ModuleKey target_module = module;
        if (resolved_callee != nullptr && !resolved_callee->target.module.empty())
            target_module = resolved_callee->target.module;

        std::vector<TypeId> explicit_types;
        explicit_types.reserve(expr.genericArgs.size());
        for (const auto generic_arg : expr.genericArgs) {
            const TypeId lowered = lowerTypeExpr(generic_arg);
            if (!lowered) {
                report(expr.span, "generic argument is not a concrete type",
                       diagnostics::err::GenericCannotInfer);
                return error_type;
            }
            explicit_types.push_back(lowered);
        }

        std::vector<TypeId> argument_types;
        argument_types.reserve(fn->params.size());
        for (size_t index = 0; index < fn->params.size(); ++index)
            argument_types.push_back(inferExpr(expr.operands[index + 1U]));

        std::vector<TypeId> args;
        const comptime::GenericResolveStatus resolved = instantiations->resolveArgs(
            *fn, generic_decl != nullptr ? generic_decl->genericParams.size() : 0U, decl_id.value,
            explicit_types, argument_types, args);
        switch (resolved) {
        case comptime::GenericResolveStatus::Arity:
            report(expr.span, "wrong generic argument count", diagnostics::err::GenericArity);
            return error_type;
        case comptime::GenericResolveStatus::CannotInfer:
            report(expr.span, "cannot infer generic argument; provide explicit type arguments",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        case comptime::GenericResolveStatus::Explosion:
            report(expr.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        case comptime::GenericResolveStatus::Ok:
            break;
        }

        const size_t instance_index =
            instantiations->bindCall(module, callee.id, target_module, decl_id, args);
        if (instance_index == ~size_t{0}) {
            report(expr.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        }
        const TypeId instance_type = instantiations->substituteFunction(*fn, args);
        const auto *instance_fn    = type_table.function(instance_type);
        for (size_t index = 0; index < fn->params.size() && instance_fn != nullptr &&
                               index < instance_fn->params.size();
             ++index) {
            TypeId arg_type = argument_types[index];
            if (!coerceValue(expr.operands[index + 1U], instance_fn->params[index], arg_type))
                reportCoercionFailure(expr.span, instance_fn->params[index], arg_type,
                                      "generic function call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
        setExprType(callee.id, instance_type);
        setResolvedCallTarget(callee.id, target_module, decl_id);
        return instance_fn != nullptr ? instance_fn->result : error_type;
    }

    const size_t checked_params =
        is_variadic ? std::min(fixed_arg_count, fn->params.size()) : fn->params.size();
    for (size_t i = 0; i < checked_params; ++i) {
        TypeId arg_type = inferExpr(expr.operands[i + 1]);
        if (!coerceValue(expr.operands[i + 1], fn->params[i], arg_type))
            reportCoercionFailure(expr.span, fn->params[i], arg_type,
                                  "function call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    // The trailing variadic arguments have no declared Zith type: infer them
    // (for diagnostics) but do not require a conversion or a fixed arity.
    for (size_t i = std::max<size_t>(1, checked_params + 1); i < expr.operands.size(); ++i)
        (void)inferExpr(expr.operands[i]);
    return fn->result;
}

/// Try to resolve `expr` (a Call whose callee is a Field/Arrow) as a
/// method call on the base type. Returns the result TypeId on success,
/// or `kInvalidTypeId` (with a diagnostic already reported) when the
/// field is not a method.
TypeId PerModuleSema::inferMethodCall(const frontend::Expression &call,
                                      const frontend::Expression &callee) {
    const TypeId base_type = inferExpr(callee.operands[0]);
    if (!base_type || type_table.kindOf(base_type) == TypeKind::Error)
        return kInvalidTypeId;

    // Unwrap pointer/optional to find the struct name. `resolve` also strips
    // memory qualifiers, so `p: lend Point` still finds Point's methods.
    TypeId pointee  = resolve(base_type);
    bool is_pointer = false;
    if (type_table.kindOf(pointee) == TypeKind::Pointer) {
        if (const auto *ptr = type_table.pointer(pointee))
            pointee = resolve(ptr->pointee);
        is_pointer = true;
    } else if (type_table.kindOf(pointee) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(pointee))
            pointee = resolve(opt->inner);
    }

    const StructType *st = type_table.struct_type(pointee);
    if (!st)
        return kInvalidTypeId; // not a struct receiver: let normal call resolution run

    // Collect every method of this owner with the callee's name: methods take part
    // in the same overload resolution as free functions.
    std::string owner_name(st->name);
    if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
        owner_name.resize(angle);
    std::vector<const frontend::Declaration *> method_decls;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        if (decl.ownerName != owner_name || decl.name != callee.text)
            continue;
        method_decls.push_back(&decl);
    }
    if (method_decls.empty())
        return kInvalidTypeId; // no such method: may still be a callable field

    if (method_decls.size() == 1U && !method_decls.front()->genericParams.empty()) {
        const frontend::Declaration *method_decl = method_decls.front();
        const auto saved_decl_id                 = currentDeclId_;
        const auto saved_kind                    = currentFunctionKind_;
        const size_t provided_args               = call.operands.size() - 1U;
        const bool has_receiver_entry =
            !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
        const size_t expected_args = has_receiver_entry ? method_decl->parameters.size() - 1U
                                                        : method_decl->parameters.size();
        if (provided_args != expected_args) {
            report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
            return error_type;
        }
        currentDeclId_       = method_decl->id.value;
        currentFunctionKind_ = method_decl->kind == frontend::DeclKind::Function
                                   ? method_decl->functionKind
                                   : frontend::FunctionKind::Standard;

        std::vector<TypeId> explicit_types;
        explicit_types.reserve(call.genericArgs.size());
        for (const auto generic_arg : call.genericArgs) {
            const TypeId lowered = lowerTypeExpr(generic_arg);
            if (!lowered) {
                report(call.span, "generic argument is not a concrete type",
                       diagnostics::err::GenericCannotInfer);
                currentDeclId_       = saved_decl_id;
                currentFunctionKind_ = saved_kind;
                return error_type;
            }
            explicit_types.push_back(lowered);
        }

        std::vector<TypeId> argument_types;
        if (has_receiver_entry) {
            const TypeId self_type =
                method_decl->parameters.front().type
                    ? lowerTypeExpr(method_decl->parameters.front().type)
                    : (is_pointer ? base_type : type_table.internPointer(pointee));
            argument_types.push_back(self_type);
        }
        for (size_t index = has_receiver_entry ? 1U : 0U; index < method_decl->parameters.size();
             ++index) {
            if (index + 1U < call.operands.size())
                argument_types.push_back(inferExpr(call.operands[index + 1U]));
        }

        const auto *method_fn = type_table.function(typeOfDecl(method_decl->id));
        std::vector<TypeId> inferred_args;
        comptime::GenericResolveStatus resolved = comptime::GenericResolveStatus::CannotInfer;
        if (method_fn != nullptr) {
            resolved =
                instantiations != nullptr
                    ? instantiations->resolveArgs(*method_fn, method_decl->genericParams.size(),
                                                  method_decl->id.value, explicit_types,
                                                  argument_types, inferred_args)
                    : comptime::GenericResolveStatus::CannotInfer;
        }
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
        switch (resolved) {
        case comptime::GenericResolveStatus::Arity:
            report(call.span, "wrong generic argument count", diagnostics::err::GenericArity);
            return error_type;
        case comptime::GenericResolveStatus::CannotInfer:
            report(call.span, "cannot infer generic argument; provide explicit type arguments",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        case comptime::GenericResolveStatus::Explosion:
            report(call.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        case comptime::GenericResolveStatus::Ok:
            break;
        }

        if (method_fn != nullptr) {
            const size_t instance_index =
                instantiations->bindCall(module, callee.id, module, method_decl->id, inferred_args);
            if (instance_index == ~size_t{0}) {
                report(call.span, "too many generic instantiations",
                       diagnostics::err::GenericExplosion);
                return error_type;
            }
            const TypeId instance_type =
                instantiations->substituteFunction(*method_fn, inferred_args);
            setExprType(callee.id, instance_type);
            setResolvedCallTarget(callee.id, module, method_decl->id);
            const auto *instance_fn = type_table.function(instance_type);
            return instance_fn != nullptr ? instance_fn->result : error_type;
        }
        return error_type;
    }

    const frontend::Declaration *method_decl = method_decls.front();
    if (method_decls.size() > 1U) {
        // The lowered method type carries the receiver as its first parameter, so
        // selection runs with one implicit argument.
        std::vector<OverloadCandidate> candidates;
        candidates.reserve(method_decls.size());
        for (const auto *decl : method_decls) {
            OverloadCandidate candidate;
            candidate.type = typeOfDecl(decl->id);
            candidate.fn   = type_table.function(candidate.type);
            candidate.span = decl->span;
            if (candidate.fn != nullptr && !typeContainsGeneric(candidate.fn))
                candidates.push_back(candidate);
        }
        if (candidates.size() > 1U) {
            bool reported      = false;
            const auto *chosen = selectOverload(call, candidates, 1U, reported);
            if (chosen == nullptr)
                return reported ? error_type : kInvalidTypeId;
            for (const auto *decl : method_decls) {
                if (decl->span == chosen->span) {
                    method_decl = decl;
                    break;
                }
            }
            setResolvedCallTarget(callee.id, module, method_decl->id);
        }
    }

    // Build the effective argument list. A method only receives an implicit
    // self argument when it actually declares a receiver parameter (either
    // `self` with no explicit type or `self: Type`). Methods declared with a
    // normal parameter list such as `fn foo(): i32` are static in this
    // compiler: `Point.foo()` passes no receiver and the resolved method
    // signature has no owner pointer.
    const auto &fn_params = method_decl->parameters;
    // A method receives an implicit self argument only when it declares a
    // receiver parameter (`self`, or `self: Type`). A method with no declared
    // parameters is static: `Type.method()` passes zero arguments.
    const bool has_receiver  = !fn_params.empty() && fn_params.front().name == "self";
    bool has_implicit_self   = has_receiver && !fn_params.front().type;
    TypeId self_type         = kInvalidTypeId;
    const auto saved_decl_id = currentDeclId_;
    const auto saved_kind    = currentFunctionKind_;
    currentDeclId_           = method_decl->id.value;
    currentFunctionKind_     = method_decl->kind == frontend::DeclKind::Function
                                   ? method_decl->functionKind
                                   : frontend::FunctionKind::Standard;
    if (has_implicit_self) {
        // First param is named self with no type: fill in *Owner.
        self_type = is_pointer ? base_type : type_table.internPointer(pointee);
    } else if (has_receiver) {
        // Explicit self param: use its declared type.
        self_type = lowerTypeExpr(fn_params.front().type);
    }

    // Check explicit arguments against remaining params.
    // A method with no receiver is static: it expects exactly the explicit
    // call arguments, so `Point.foo()` resolves with zero args.
    const size_t expected_args = has_receiver ? fn_params.size() - 1 : fn_params.size();
    const size_t provided_args = call.operands.size() - 1;
    if (provided_args != expected_args) {
        report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
        return error_type;
    }
    for (size_t i = 0; i < provided_args; ++i) {
        TypeId arg_type = inferExpr(call.operands[i + 1]);
        TypeId param_type =
            i + 1 < fn_params.size() ? lowerTypeExpr(fn_params[i + 1].type) : error_type;
        if (param_type && arg_type && !coerceValue(call.operands[i + 1], param_type, arg_type))
            reportCoercionFailure(call.span, param_type, arg_type,
                                  "method call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }

    TypeId result = lowerTypeExpr(method_decl->declaredType);
    if (!result)
        result = void_type;
    currentDeclId_       = saved_decl_id;
    currentFunctionKind_ = saved_kind;
    // Record a type for the Field/Arrow callee: it is a resolved method, not a
    // struct field, and the later standalone-expression sweep would otherwise
    // re-infer it as a field access and report "unknown field".
    if (const TypeId method_type = typeOfDecl(method_decl->id))
        setExprType(callee.id, method_type);
    else
        setExprType(callee.id, result);
    return result;
}

bool PerModuleSema::typeContainsGeneric(const FunctionType *fn) const noexcept {
    for (const auto param : fn->params)
        if (type_table.kindOf(param) == TypeKind::GenericParam)
            return true;
    return type_table.kindOf(fn->result) == TypeKind::GenericParam;
}

bool PerModuleSema::bindingIsVariadic(const session::ResolvedName &binding) noexcept {
    return binding.isVariadic ||
           (binding.foreignFunction != nullptr && binding.foreignFunction->isVariadic);
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
        if (snapshot.isMacroTemplateStmt(stmt.id))
            continue;
        if (stmt.kind == frontend::StmtKind::Expression && stmt.expression) {
            last = inferExpr(stmt.expression);
        } else if (stmt.kind == frontend::StmtKind::Binding) {
            if (currentStacklessMarker_) {
                report(stmt.span, "bindings are not allowed in a stackless marker",
                       diagnostics::err::UnsupportedSyntax);
            }
            TypeId ann_type = lowerTypeExpr(stmt.binding.type);
            TypeId init_type =
                stmt.binding.initializer ? inferExpr(stmt.binding.initializer) : invalid_type;
            if (ann_type && stmt.binding.initializer &&
                !coerceValue(stmt.binding.initializer, ann_type, init_type)) {
                reportCoercionFailure(stmt.span, ann_type, init_type,
                                      "binding initializer type does not match annotation");
            }
            if (!ann_type && stmt.binding.initializer) {
                if (resolve(init_type) == null_type) {
                    report(stmt.span, "null requires an optional type annotation",
                           diagnostics::err::TypeMismatch);
                    init_type = error_type;
                }
            }
            setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Return) {
            checkReturnStatement(stmt);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Marker) {
            if (currentFunctionKind_ != frontend::FunctionKind::Flow) {
                report(stmt.span, "marker is only allowed inside a flow fn",
                       diagnostics::err::UnsupportedSyntax);
            }
            if (!inMarkerBody_) {
                for (const auto &param : stmt.parameters)
                    (void)markerParamType(param);
                if (stmt.expression) {
                    const bool saved_marker = inMarkerBody_;
                    const bool saved_global = inGlobalMarker_;
                    inMarkerBody_           = true;
                    currentStacklessMarker_ = !stmt.isStackful;
                    (void)inferExpr(stmt.expression);
                    currentStacklessMarker_ = false;
                    inGlobalMarker_         = saved_global;
                    inMarkerBody_           = saved_marker;
                }
            }
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Dock) {
            if (currentFunctionKind_ != frontend::FunctionKind::Flow) {
                report(stmt.span, "dock is only allowed inside a flow fn",
                       diagnostics::err::UnsupportedSyntax);
            }
            if (inMarkerBody_)
                report(stmt.span, "dock is only allowed outside a marker body",
                       diagnostics::err::UnsupportedSyntax);
            if (!stmt.label.empty())
                validateMarkerReference(stmt.span, stmt.label, stmt);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Jump) {
            if (currentFunctionKind_ != frontend::FunctionKind::Flow) {
                report(stmt.span, "jump is only allowed inside a flow fn",
                       diagnostics::err::UnsupportedSyntax);
            }
            if (!inMarkerBody_) {
                report(stmt.span, "jump is only allowed inside a marker body",
                       diagnostics::err::UnsupportedSyntax);
            }
            validateMarkerReference(stmt.span, stmt.label, stmt);
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
    const auto &condition = snapshot.expressions()[expr.operands[0].value - 1U];
    frontend::LocalId narrowed_local;
    TypeId original_local_type = kInvalidTypeId;
    TypeId narrowed_type       = kInvalidTypeId;
    if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
        condition.cast_type) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            narrowed_local      = resolved->local;
            original_local_type = typeOfLocal(narrowed_local);
            narrowed_type       = lowerTypeExpr(condition.cast_type);
            if (narrowed_type)
                setLocalType(narrowed_local, narrowed_type);
        }
    }
    TypeId then_type = inferExpr(expr.operands[1]);
    if (narrowed_local && narrowed_type) {
        setLocalType(narrowed_local, original_local_type);
    }
    TypeId else_type = expr.operands.size() >= 3 ? inferExpr(expr.operands[2]) : void_type;
    // An `if` without `else` is a statement even when its body has a value; only
    // an `if/else` expression can produce a value for the surrounding expression.
    if (expr.operands.size() < 3U || !expr.operands[2])
        return void_type;
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
        if (type_table.kindOf(from_resolved) == TypeKind::Union)
            return unionMemberType(expr.span, from_resolved, to_resolved);
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

TypeId PerModuleSema::inferIsType(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty() || !expr.cast_type)
        return error_type;
    const TypeId operand = inferExpr(expr.operands[0]);
    if (operand == error_type || !operand)
        return error_type;
    const TypeId operand_resolved = resolve(operand);
    const auto *union_data        = type_table.union_type(operand_resolved);
    if (union_data == nullptr || !union_data->is_tagged) {
        report(expr.span, "'is Type' requires an operand whose type is a tagged union",
               diagnostics::err::TypeMismatch);
        return error_type;
    }
    const TypeId target = lowerTypeExpr(expr.cast_type);
    if (!target) {
        report(expr.span, "unknown target type in 'is' test", diagnostics::err::TypeMismatch);
        return error_type;
    }
    for (const auto member : union_data->members) {
        if (sameType(resolve(member), resolve(target)))
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

void PerModuleSema::checkReturnStatement(const frontend::Statement &stmt) {
    if (inGlobalMarker_) {
        report(stmt.span, "return is not allowed in a global marker",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
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
    if (inGlobalMarker_) {
        const auto &expr = snapshot.expressions()[id.value - 1U];
        report(expr.span, "return is not allowed in a global marker",
               diagnostics::err::UnsupportedSyntax);
        return type_table.internName("never", TypeKind::Never);
    }
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId value     = expr.operands.empty() ? void_type : inferExpr(expr.operands[0]);
    if (currentReturnType_ && value && value != error_type && !expr.operands.empty() &&
        !coerceValue(expr.operands[0], currentReturnType_, value)) {
        reportCoercionFailure(expr.span, currentReturnType_, value,
                              "return type does not match declared return type");
    }
    return type_table.internName("never", TypeKind::Never);
}

frontend::ExprId PerModuleSema::assignmentRoot(frontend::ExprId id) const noexcept {
    // Walk the place expression down to the name it is rooted at, so a write to
    // `p.inner.x` is attributed to the binding `p`.
    for (unsigned guard = 0; guard < 64U; ++guard) {
        if (!id || id.value > snapshot.expressions().size())
            return {};
        const auto &expr = snapshot.expressions()[id.value - 1U];
        switch (expr.kind) {
        case frontend::ExprKind::Name:
            return id;
        case frontend::ExprKind::Field:
        case frontend::ExprKind::Arrow:
        case frontend::ExprKind::Index:
            if (expr.operands.empty())
                return {};
            id = expr.operands[0];
            break;
        case frontend::ExprKind::Unary:
            if (expr.text != "*" || expr.operands.empty())
                return {};
            id = expr.operands[0];
            break;
        default:
            return {};
        }
    }
    return {};
}

void PerModuleSema::checkAssignableOwnership(frontend::ExprId target, frontend::TextSpan span) {
    const frontend::ExprId root = assignmentRoot(target);
    if (!root)
        return;
    const TypeId declared = typeOfExpr(root) ? typeOfExpr(root) : typeOfResolvedName(root);
    if (!declared)
        return;
    const auto *qual = type_table.qualified(type_table.canonical(declared));
    if (qual == nullptr || qual->ownership != types::OwnershipKind::View)
        return;
    const auto &root_expr = snapshot.expressions()[root.value - 1U];
    report(span, "cannot write through '" + root_expr.text + "': a 'view' binding is read-only",
           diagnostics::err::WriteThroughView);
}

void PerModuleSema::checkImmutableRootFieldWrite(frontend::ExprId target, frontend::TextSpan span) {
    const frontend::ExprId root = assignmentRoot(target);
    const auto *root_resolved   = root ? findResolvedExpr(root) : nullptr;
    if (root_resolved == nullptr ||
        (root_resolved->bindingKind != frontend::BindingKind::Let &&
         root_resolved->bindingKind != frontend::BindingKind::Const) ||
        root_resolved->declKind == frontend::DeclKind::Function ||
        root_resolved->declKind == frontend::DeclKind::Marker)
        return;
    bool has_field_path = false;
    for (frontend::ExprId current = target;
         current && current.value <= snapshot.expressions().size() && current != root;) {
        const auto &cursor = snapshot.expressions()[current.value - 1U];
        if (cursor.kind != frontend::ExprKind::Field && cursor.kind != frontend::ExprKind::Arrow &&
            cursor.kind != frontend::ExprKind::Index)
            break;
        if (cursor.operands.empty())
            break;
        has_field_path = true;
        current        = cursor.operands[0];
    }
    if (!has_field_path)
        return;
    report(span, "Zith--: cannot write through immutable binding '" + root_resolved->name + "'",
           diagnostics::err::UnsupportedSyntax);
}

TypeId PerModuleSema::inferAssign(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    TypeId left_type = kInvalidTypeId;
    prepareLValueIndexTypes(expr.operands[0]);
    const auto *left_resolved = findResolvedExpr(expr.operands[0]);
    if (left_resolved != nullptr) {
        const auto isFirstAssignmentForLet = [&]() {
            if (!left_resolved || !left_resolved->local)
                return false;
            if (std::find(typeInferredByAssignment_.begin(), typeInferredByAssignment_.end(),
                          left_resolved->local.value) != typeInferredByAssignment_.end())
                return false;
            for (const auto &statement : snapshot.statements()) {
                if (statement.kind == frontend::StmtKind::Binding &&
                    statement.binding.id == left_resolved->local &&
                    !statement.binding.initializer) {
                    return true;
                }
            }
            return false;
        };
        const bool first_assignment = isFirstAssignmentForLet();
        if ((left_resolved->bindingKind == frontend::BindingKind::Let ||
             left_resolved->bindingKind == frontend::BindingKind::Const)) {
            if (first_assignment && left_resolved->bindingKind == frontend::BindingKind::Let) {
                // `let x; x = e;` is allowed once; it supplies the variable's type.
                typeInferredByAssignment_.push_back(left_resolved->local.value);
            } else {
                report(expr.span, "Zith--: cannot assign to an immutable let/const binding",
                       diagnostics::err::UnsupportedSyntax);
            }
        }
        if (left_resolved->declaration && left_resolved->declKind == frontend::DeclKind::Variable &&
            left_resolved->bindingKind == frontend::BindingKind::Const) {
            report(expr.span, "Zith--: cannot assign to a const global",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    if (left_resolved != nullptr && left_resolved->local)
        left_type = typeOfLocal(left_resolved->local);
    if (!left_type && left_resolved != nullptr && left_resolved->declaration)
        left_type = typeOfDecl(left_resolved->declaration);
    if (!left_type)
        left_type = inferExpr(expr.operands[0]);
    if (!left_type)
        left_type = error_type;

    if (resolve(left_type) == invalid_type) {
        // A binding declared `let x;` is typed by its first assignment.
        TypeId narrowed = inferExpr(expr.operands[1]);
        if (narrowed) {
            if (left_resolved != nullptr && left_resolved->local)
                setLocalType(left_resolved->local, narrowed);
            left_type = narrowed;
        }
    }
    if (expr.operands[0] && !typeOfExpr(expr.operands[0])) {
        (void)inferExpr(expr.operands[0]);
    }
    TypeId right_type = inferExpr(expr.operands[1]);
    checkAssignableOwnership(expr.operands[0], expr.span);
    checkImmutableRootFieldWrite(expr.operands[0], expr.span);
    TypeId result = left_type;
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
    if (is_array) {
        if (const auto *array = type_table.array(resolved_object)) {
            element       = array->element;
            object_length = array->size;
        }
    } else if (is_slice) {
        if (const auto *slice = type_table.slice(resolved_object))
            element = slice->element;
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
    const auto *st     = type_table.struct_type(resolved);
    if (st == nullptr) {
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
    return st->fields[static_cast<size_t>(idx)];
}

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

        const auto &operand = snapshot.expressions()[expr.operands[i].value - 1U];
        if (operand.kind == frontend::ExprKind::Placeholder) {
            if (!findFieldDefault(template_decl.name, static_cast<size_t>(decl_idx))) {
                report(expr.span,
                       "field '" + template_decl.parameters[static_cast<size_t>(decl_idx)].name +
                           "' has no default value for '_'",
                       diagnostics::err::TypeMismatch);
            }
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
        }
    }

    for (size_t i = 0; i < field_count; ++i) {
        if (seen[i] || findFieldDefault(template_decl.name, i))
            continue;
        report(expr.span,
               "missing field '" + template_decl.parameters[i].name +
                   "' in struct literal; add a value or a field default",
               diagnostics::err::TypeMismatch);
    }
    return TypeId{concrete_resolved.intern_seq};
}

TypeId PerModuleSema::inferStructLiteral(frontend::ExprId id) {
    const auto &expr        = snapshot.expressions()[id.value - 1U];
    std::string struct_name = expr.text;
    TypeId struct_tid       = kInvalidTypeId;
    TypeId resolved         = kInvalidTypeId;
    const StructType *st    = nullptr;
    bool from_generic_args  = false;
    if (!expr.genericArgs.empty()) {
        from_generic_args         = true;
        const TypeId instantiated = instantiateTypeExpr(expr.span, expr.text, expr.genericArgs);
        if (!instantiated) {
            return error_type;
        }
        if (type_table.struct_type(type_table.stripQualifiers(instantiated)) == nullptr) {
            report(expr.span, "'" + expr.text + "' is not a generic struct type",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        struct_tid = instantiated;
        resolved   = type_table.stripQualifiers(struct_tid);
        st         = type_table.struct_type(resolved);
    }
    if (!from_generic_args) {
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
    if (!from_generic_args) {
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
        if (!binding.initializer &&
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
        if (type_table.kindOf(resolved_target) == TypeKind::Optional) {
            if (resolve(source) == null_type) {
                result = true;
            } else if (const auto *opt = type_table.optional(resolved_target)) {
                result = sameType(opt->inner, source);
            }
        } else {
            result = allowsUncheckedNullablePointer(target, source);
        }
        // Any pointer (or nullable pointer) reaches a `void*` parameter without a cast, so
        // `free(x)` works for both `*i32` and `?*i32`. The reverse still needs `as`.
        if (!result && isVoidPointer(resolved_target) && pointerBase(source))
            result = true;
        // A fixed array is an implicit view into a slice of the same element type.
        const TypeId resolved_source = resolve(source);
        if (!result && type_table.kindOf(resolved_target) == TypeKind::Slice) {
            const auto *slice = type_table.slice(resolved_target);
            const auto *array = type_table.array(resolved_source);
            result =
                slice != nullptr && array != nullptr && sameType(slice->element, array->element);
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

const frontend::Expression *PerModuleSema::nameExpression(frontend::ScopeId scope,
                                                          std::string_view name) const noexcept {
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Name && expression.text == name &&
            expression.scope == scope && findResolvedExpr(expression.id) != nullptr &&
            findResolvedExpr(expression.id)->local) {
            return &expression;
        }
    }
    return nullptr;
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
    return session::lookupExprResolution(resolution, id);
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
    return memory::Span{fileId, span.start, span.end};
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

        auto *sema =
            arena_.make<PerModuleSema>(artifact.key, *artifact.frontend, *resolution, type_table_,
                                       *typed_map, arena_, artifact.fileId, this);
        sema->instantiations = instantiation_pass_;
        modules_.push(sema);
        if (!sema->prepareTypes())
            has_errors_ = true;
    }
    for (auto *sema : modules_) {
        // Imported modules are declaration sources, not entry programs: their
        // public bodies become real code only when expanded/called from the
        // root module, so only the root is expression-checked here.
        if (sema->module == snapshot_.rootModuleKey())
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
