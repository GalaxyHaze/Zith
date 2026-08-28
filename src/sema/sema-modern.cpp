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
      f32_type(kInvalidTypeId), f64_type(kInvalidTypeId), null_type(kInvalidTypeId),
      end_type(kInvalidTypeId) {}

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
    checkImplementBlocks();
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
    end_type     = type_table.lookupNamed("End");
    if (!end_type)
        end_type = type_table.findOrCreateNamed("End", TypeKind::Struct);
    if (type_table.struct_type(end_type) == nullptr) {
        // The canonical marker is a real zero-field struct so `End {}` is
        // constructible in any module without a user-level declaration.
        auto &fields = type_table.makeTypeStorage();
        auto &names  = type_table.makeStringStorage();
        end_type     = type_table.internStruct("End", fields, &names);
        type_table.registerNamed("End", end_type);
    }
    void_type = registerPrimitive("void", TypeKind::Void, 0, false);
    bool_type = registerPrimitive("bool", TypeKind::Bool, 0, false);
    char_type = registerPrimitive("char", TypeKind::Char, 0, false);

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
        case frontend::DeclKind::Interface:
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
                GenericBinding binding;
                binding.name = decl.genericParams[i].name;
                binding.type = param_type;
                bindings.push_back(std::move(binding));
            }
            // Install the full name->GenericParam table before lowering bounds,
            // so a bound like `T: Foo` can resolve `T` if it appears in its own
            // constraint expression (e.g. a constrained generic alias).
            genericParams_[decl.id.value] = bindings;
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
                            if (binding.name == owner_decl.genericParams[index].name) {
                                binding.type = type_table.internGenericParam(
                                    owner_decl.id.value, static_cast<uint32_t>(index));
                                binding.bounds.clear();
                            }
                        }
                    }
                    break;
                }
            }
            for (size_t i = 0; i < bindings.size(); ++i) {
                for (const auto constraint : decl.genericParams[i].constraints) {
                    const TypeId bound = lowerTypeExpr(constraint);
                    if (bound)
                        bindings[i].bounds.push_back(bound);
                }
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
                            if (bindings[i].name == owner_decl.genericParams[index].name) {
                                for (const auto constraint :
                                     owner_decl.genericParams[index].constraints) {
                                    const TypeId bound = lowerTypeExpr(constraint);
                                    if (bound)
                                        bindings[i].bounds.push_back(bound);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            genericParams_[decl.id.value] = std::move(bindings);
        }
        switch (decl.kind) {
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
                if (is_method && i == 0 && param.name == "self" && owner_type) {
                    if (!decl.parameters.front().type) {
                        // `self` (without a type) is shorthand for `*Owner`.
                        ptype = type_table.internPointer(owner_type);
                    } else {
                        ptype = methodSelfParamType(param);
                    }
                } else {
                    ptype = borrowParamType(param);
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
            auto &fields     = type_table.makeTypeStorage();
            auto &fld_names  = type_table.makeStringStorage();
            auto &field_meta = type_table.makeFieldMetaStorage();
            // Intern each field's name (as arena string_view) and type
            for (const auto &param : decl.parameters) {
                TypeId ftype = lowerTypeExpr(param.type);
                if (!ftype)
                    ftype = error_type;
                fields.push(ftype);
                field_meta.push(FieldMeta{param.visibility, param.modDepth, module});
                // Store name in a stable arena allocation
                char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
                std::memcpy(buf, param.name.data(), param.name.size());
                fld_names.push(std::string_view(buf, param.name.size()));
            }
            TypeId st = type_table.internStruct(decl.name, fields, &fld_names, &field_meta);
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
                    const std::int64_t min = std::numeric_limits<std::int64_t>::min();
                    const std::int64_t max = std::numeric_limits<std::int64_t>::max();

                    if (expr.text == "+") {
                        if (right > 0 && left > max - right)
                            return false;
                        if (right < 0 && left < min - right)
                            return false;
                        out = left + right;
                        return true;
                    }
                    if (expr.text == "-") {
                        if (right < 0 && left > max + right)
                            return false;
                        if (right > 0 && left < min + right)
                            return false;
                        out = left - right;
                        return true;
                    }
                    if (expr.text == "*") {
                        if (right == 0) {
                            out = 0;
                            return true;
                        }
                        const auto magnitude = [](std::int64_t value) {
                            return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                             : static_cast<std::uint64_t>(value);
                        };
                        const std::uint64_t left_mag  = magnitude(left);
                        const std::uint64_t right_mag = magnitude(right);
                        const std::uint64_t limit     = ((left < 0) == (right < 0))
                                                            ? static_cast<std::uint64_t>(max)
                                                            : (std::uint64_t{1} << 63U);
                        if (left_mag > limit / right_mag)
                            return false;
                        const std::uint64_t product = left_mag * right_mag;
                        if ((left < 0) == (right < 0)) {
                            out = static_cast<std::int64_t>(product);
                        } else {
                            out = product == (std::uint64_t{1} << 63U)
                                      ? min
                                      : -static_cast<std::int64_t>(product);
                        }
                        return true;
                    }
                    if (expr.text == "/" || expr.text == "%") {
                        out = expr.text == "/" ? left / right : left % right;
                        return true;
                    }
                    if (expr.text == "&.") {
                        out = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) &
                                                        static_cast<std::uint64_t>(right));
                        return true;
                    }
                    if (expr.text == "|.") {
                        out = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) |
                                                        static_cast<std::uint64_t>(right));
                        return true;
                    }
                    if (expr.text == "^.") {
                        out = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) ^
                                                        static_cast<std::uint64_t>(right));
                        return true;
                    }
                    if (expr.text == "<<") {
                        if (right < 0 || right >= 63)
                            return false;
                        const std::uint64_t shifted =
                            static_cast<std::uint64_t>(static_cast<std::int64_t>(left))
                            << static_cast<unsigned>(right);
                        if (shifted > static_cast<std::uint64_t>(max))
                            return false;
                        out = static_cast<std::int64_t>(shifted);
                        return true;
                    }
                    if (expr.text == ">>") {
                        out = left >> static_cast<unsigned>(right);
                        return true;
                    }
                    if (expr.text == "==" || expr.text == "!=" || expr.text == "<" ||
                        expr.text == "<=" || expr.text == ">" || expr.text == ">=") {
                        out = expr.text == "=="   ? (left == right)
                              : expr.text == "!=" ? (left != right)
                              : expr.text == "<"  ? (left < right)
                              : expr.text == "<=" ? (left <= right)
                              : expr.text == ">"  ? (left > right)
                                                  : (left >= right);
                        return true;
                    }
                    return false;
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
        case frontend::DeclKind::Interface: {
            TypeId it = type_table.internInterface(decl.name);
            setDeclType(decl.id, it);
            type_table.registerNamed(decl.name, it);
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
    stateMachineByDecl_.clear();
    stateMachineByReturn_.clear();
    localStateMachineByParent_.clear();
    nextStateMachineId_        = 1;
    bool has_non_template_code = false;
    for (const auto &decl : snapshot.declarations()) {
        // A macro declaration is a template, not code: its body only becomes
        // real code once cloned into a call site.
        if (decl.kind == frontend::DeclKind::Macro)
            continue;
        if (decl.kind != frontend::DeclKind::Import)
            has_non_template_code = true;
        if (decl.kind == frontend::DeclKind::Function &&
            decl.functionKind == frontend::FunctionKind::State && !decl.parentScope) {
            (void)stateMachineIdFor(decl);
        }
        if (decl.kind == frontend::DeclKind::Function && decl.parentScope &&
            decl.functionKind == frontend::FunctionKind::State) {
            // Local states form one machine per parent body, independent of the
            // top-level return-type grouping.
            const auto [it, inserted] =
                localStateMachineByParent_.emplace(decl.parentScope.value, 0U);
            if (inserted)
                it->second = nextStateMachineId_++;
            stateMachineByDecl_[decl.id.value] = it->second;
        }
        currentDeclId_       = decl.id.value;
        currentFunctionKind_ = decl.kind == frontend::DeclKind::Function
                                   ? decl.functionKind
                                   : frontend::FunctionKind::Standard;
        currentBodyScope_    = decl.kind == frontend::DeclKind::Function && decl.body
                                   ? snapshot.expressions()[decl.body.value - 1U].scope
                                   : frontend::ScopeId{};
        movedLocals_.clear();
        inStateBody_ = decl.kind == frontend::DeclKind::Function &&
                       decl.functionKind == frontend::FunctionKind::State;
        currentStateMachineId_ =
            inStateBody_ ? (decl.parentScope ? stateMachineIdOf(decl) : stateMachineIdFor(decl))
                         : 0;
        if (decl.kind == frontend::DeclKind::Function) {
            TypeId fn_type     = typeOfDecl(decl.id);
            const auto *fn     = type_table.function(fn_type);
            currentReturnType_ = fn ? fn->result : kInvalidTypeId;
        } else {
            currentReturnType_ = kInvalidTypeId;
        }
        if (decl.body) {
            currentDeclId_ = decl.id.value;
            if (decl.parentScope && decl.functionKind == frontend::FunctionKind::State) {
                currentStateMachineId_ = stateMachineIdOf(decl);
            }
            (void)inferExpr(decl.body);
        }
        if (decl.initializer) {
            (void)inferExpr(decl.initializer);
        }
    }
    if (!has_non_template_code) {
        currentDeclId_         = 0;
        currentBodyScope_      = {};
        currentFunctionKind_   = frontend::FunctionKind::Standard;
        inStateBody_           = false;
        currentStateMachineId_ = 0;
        return;
    }
    // Infer iterator loops before the standalone sweep: the synthetic binding
    // in the loop body must be typed by the iterable before a body read is
    // visited as an independent expression.
    for (const auto &expr : snapshot.expressions()) {
        if (snapshot.isMacroTemplateExpr(expr.id))
            continue;
        if (expr.kind == frontend::ExprKind::ForIn) {
            setExprType(expr.id, inferForIn(expr.id));
        }
    }
    // Also infer standalone expressions, skipping macro template bodies.  The
    // expanded clones of imported state declarations are validated in their
    // own module context, so a detached clone re-validated out of order does
    // not carry this module's state-machine grouping.
    for (const auto &expr : snapshot.expressions()) {
        if (snapshot.isMacroTemplateExpr(expr.id))
            continue;
        if (!typeOfExpr(expr.id))
            (void)inferExpr(expr.id);
    }
    currentDeclId_         = 0;
    currentBodyScope_      = {};
    currentFunctionKind_   = frontend::FunctionKind::Standard;
    inStateBody_           = false;
    currentStateMachineId_ = 0;
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
            TypeId body_type         = typeOfExpr(decl.body);
            const auto *body_expr    = decl.body.value <= snapshot.expressions().size()
                                           ? &snapshot.expressions()[decl.body.value - 1U]
                                           : nullptr;
            const bool bodyHasReturn = [&]() {
                if (body_expr == nullptr || body_expr->kind != frontend::ExprKind::Block)
                    return false;
                for (const frontend::StmtId stmt_id : body_expr->statements) {
                    if (!stmt_id || stmt_id.value > snapshot.statements().size())
                        continue;
                    if (snapshot.statements()[stmt_id.value - 1U].kind ==
                        frontend::StmtKind::Return)
                        return true;
                }
                return false;
            }();
            const bool bodyEndsWithStateTransfer = [&]() {
                if (decl.functionKind != frontend::FunctionKind::State || body_expr == nullptr ||
                    body_expr->kind != frontend::ExprKind::Block) {
                    return false;
                }
                // A `jump` is a terminating transfer for the state body even
                // when it is not the literally last statement (after binds,
                // expressions, `defer`, or trailing declarations).
                for (const frontend::StmtId stmt_id : body_expr->statements) {
                    if (!stmt_id || stmt_id.value > snapshot.statements().size())
                        continue;
                    if (snapshot.statements()[stmt_id.value - 1U].kind ==
                        frontend::StmtKind::Jump) {
                        return true;
                    }
                    if (snapshot.statements()[stmt_id.value - 1U].kind ==
                        frontend::StmtKind::Return) {
                        return false;
                    }
                }
                return false;
            }();
            if (!sameType(body_type, void_type) && ret_type != void_type &&
                !coercesTo(ret_type, body_type)) {
                reportCoercionFailure(snapshot.expressions()[decl.body.value - 1U].span, ret_type,
                                      body_type,
                                      "function body type does not match declared return type");
            } else if (sameType(body_type, void_type) && !bodyHasReturn && ret_type != void_type &&
                       ret_type != error_type && !bodyEndsWithStateTransfer) {
                reportCoercionFailure(
                    snapshot.expressions()[decl.body.value - 1U].span, ret_type, body_type,
                    "function body is missing a value of the declared return type");
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

void PerModuleSema::checkImplementBlocks() {
    // Group method declarations by `(owner, trait)` so an empty implement block
    // is still checked for duplicate implementation and missing requirements.
    struct ImplGroup {
        const frontend::Declaration *owner = nullptr;
        const frontend::Declaration *trait = nullptr;
        std::vector<const frontend::Declaration *> methods;
        frontend::TextSpan span;
        std::vector<frontend::TextSpan> spans;
    };
    std::unordered_map<uint64_t, ImplGroup> groups;

    for (const auto &record : snapshot.implementRecords()) {
        const uint64_t key =
            (static_cast<uint64_t>(std::hash<std::string_view>{}(record.owner)) << 32U) ^
            static_cast<uint32_t>(std::hash<std::string_view>{}(record.traitName));
        const frontend::Declaration *decl_owner = nullptr;
        for (const auto &candidate : snapshot.declarations()) {
            if (candidate.name == record.owner &&
                (candidate.kind == frontend::DeclKind::Struct ||
                 candidate.kind == frontend::DeclKind::Enum ||
                 candidate.kind == frontend::DeclKind::Union ||
                 candidate.kind == frontend::DeclKind::TypeAlias)) {
                decl_owner = &candidate;
                break;
            }
        }
        const frontend::Declaration *trait = nullptr;
        for (const auto &candidate : snapshot.declarations()) {
            if (candidate.name == record.traitName &&
                (candidate.kind == frontend::DeclKind::Trait ||
                 candidate.kind == frontend::DeclKind::Interface)) {
                trait = &candidate;
                break;
            }
        }
        auto &group = groups[key];
        if (group.trait == nullptr) {
            group.span  = record.span;
            group.owner = decl_owner;
            group.trait = trait;
        }
        group.spans.push_back(record.span);
    }
    // Attach methods to the implementation group. A declaration's `traitName`
    // is non-empty only for methods lowered from an implement block.
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || decl.traitName.empty())
            continue;
        const uint64_t key =
            (static_cast<uint64_t>(std::hash<std::string_view>{}(decl.ownerName)) << 32U) ^
            static_cast<uint32_t>(std::hash<std::string_view>{}(decl.traitName));
        auto found = groups.find(key);
        if (found != groups.end())
            found->second.methods.push_back(&decl);
    }

    for (auto &entry : groups) {
        auto &group = entry.second;
        if (group.trait == nullptr) {
            report(group.span, "type after 'as'/'for' is not a declared trait or interface",
                   diagnostics::err::NotATrait);
            continue;
        }
        if (group.owner == nullptr)
            continue;
        if (group.spans.size() > 1U) {
            report(group.spans[1],
                   "duplicate implementation of trait '" + group.trait->name + "' for type '" +
                       group.owner->name + "'",
                   diagnostics::err::DuplicateImplementation);
            continue;
        }
        if (group.trait->kind == frontend::DeclKind::Interface) {
            report(group.span, "interfaces are structural and cannot be implemented explicitly",
                   diagnostics::err::InterfaceMethodNotAllowed);
            continue;
        }

        const TypeId owner_type = type_table.lookupNamed(group.owner->name);
        const TypeId trait_type = type_table.lookupNamed(group.trait->name);
        if (!owner_type || !trait_type)
            continue;
        // Validate every trait requirement without a default against the impl.
        for (const auto &requirement : snapshot.declarations()) {
            if (requirement.kind != frontend::DeclKind::Function ||
                requirement.ownerName != group.trait->name)
                continue;
            if (requirement.body)
                continue;

            const frontend::Declaration *impl = nullptr;
            for (const auto *candidate : group.methods) {
                if (candidate->name != requirement.name)
                    continue;
                impl = candidate;
                break;
            }
            if (impl == nullptr) {
                report(group.span,
                       "trait '" + group.trait->name + "' requires method '" + requirement.name +
                           "' that is not implemented",
                       diagnostics::err::TraitRequirementMissing);
                continue;
            }

            const TypeId impl_fn  = typeOfDecl(impl->id);
            const TypeId req_fn   = typeOfDecl(requirement.id);
            const auto *impl_type = type_table.function(impl_fn);
            const auto *req_type  = type_table.function(req_fn);
            if (impl_type == nullptr || req_type == nullptr)
                continue;

            bool matched = impl_type->params.size() == req_type->params.size() &&
                           sameType(substituteSelf(impl_type->result, owner_type, trait_type),
                                    substituteSelf(req_type->result, owner_type, trait_type));
            if (matched) {
                for (size_t index = 0; index < impl_type->params.size(); ++index) {
                    if (!sameType(
                            substituteSelf(impl_type->params[index], owner_type, trait_type),
                            substituteSelf(req_type->params[index], owner_type, trait_type))) {
                        matched = false;
                        break;
                    }
                }
            }
            if (!matched) {
                report(impl->span,
                       "method '" + impl->name + "' does not match trait '" + group.trait->name +
                           "' requirement signature",
                       diagnostics::err::TraitMethodSignatureMismatch);
            }
        }

        type_table.conformanceTable().registerConformance(owner_type, trait_type);

        // Expose the trait's default methods as owner calls. The signature is
        // substituted with the concrete owner for `Self`; the declaration stays
        // the trait's so HIR can lower its body from the defining module.
        for (const auto &default_method : snapshot.declarations()) {
            if (default_method.kind != frontend::DeclKind::Function ||
                default_method.ownerName != group.trait->name)
                continue;
            if (!default_method.body)
                continue;
            const TypeId default_type = typeOfDecl(default_method.id);
            if (type_table.function(default_type) == nullptr)
                continue;
            (void)substituteSelf(default_type, owner_type, trait_type);
        }
    }
}

TypeId PerModuleSema::substituteSelf(TypeId type, TypeId self, TypeId trait) const {
    if (!type)
        return type;
    if (const auto *qualified = type_table.qualified(type))
        return type_table.internQualified(substituteSelf(qualified->inner, self, trait),
                                          qualified->ownership, qualified->isMut);
    if (const auto *alias = type_table.alias(type))
        return type_table.internAlias(substituteSelf(alias->target, self, trait));
    if (const auto *nominal = type_table.nominal(type))
        return type_table.internNominal(nominal->name,
                                        substituteSelf(nominal->target, self, trait));
    const TypeId resolved = type_table.stripQualifiers(type);
    if (const auto *ptr = type_table.pointer(resolved))
        return type_table.internPointer(substituteSelf(ptr->pointee, self, trait));
    if (const auto *opt = type_table.optional(resolved))
        return type_table.internOptional(substituteSelf(opt->inner, self, trait));
    if (const auto *array = type_table.array(resolved))
        return type_table.internArray(substituteSelf(array->element, self, trait), array->size);
    if (const auto *slice = type_table.slice(resolved))
        return type_table.internSlice(substituteSelf(slice->element, self, trait));
    if (const auto *fn = type_table.function(resolved)) {
        auto &params = type_table.makeTypeStorage();
        for (const auto param : fn->params)
            params.push(substituteSelf(param, self, trait));
        return type_table.internFunction(params, substituteSelf(fn->result, self, trait));
    }
    if (trait && resolve(type) == resolve(trait))
        return self;
    return type;
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
        // `[...]T` and `[]T` share the same runtime slice type. The parser
        // records `isVariadicSlice` on the declaration so call resolution can
        // collect extra homogeneous arguments into the slice at the call site.
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
    case frontend::TypeExprKind::OpaqueTagged:
        return type_table.internOpaque();
    case frontend::TypeExprKind::Pack: {
        if (type.member_names.size() != type.arguments.size()) {
            report(type.span, "pack type members must be named", diagnostics::err::TypeMismatch);
            return error_type;
        }
        auto &members = type_table.makeTypeStorage();
        auto &names   = type_table.makeStringStorage();
        for (const auto &arg : type.arguments)
            members.push(lowerTypeExpr(arg));
        for (const auto &name : type.member_names)
            names.push(name);
        for (size_t i = 0; i < names.size(); ++i)
            for (size_t j = i + 1U; j < names.size(); ++j)
                if (names[i] == names[j]) {
                    report(type.span, "duplicate pack member name '" + std::string(names[i]) + "'",
                           diagnostics::err::TypeMismatch);
                    return error_type;
                }
        return type_table.internPack(members, names);
    }
    case frontend::TypeExprKind::Dyn: {
        if (type.arguments.empty())
            return error_type;
        const TypeId target = resolve(lowerTypeExpr(type.arguments[0]));
        const auto *tr      = type_table.trait(target);
        if (tr == nullptr) {
            report(type.span, "'dyn' target must be a trait or interface",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        const frontend::Declaration *decl = findDeclNamed(
            tr->name, findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr
                          ? frontend::DeclKind::Interface
                          : frontend::DeclKind::Trait);
        const bool is_iface = findDeclNamed(tr->name, frontend::DeclKind::Interface) != nullptr;
        size_t methods      = 0;
        if (decl != nullptr) {
            for (const auto &candidate : snapshot.declarations()) {
                if (candidate.kind == frontend::DeclKind::Function &&
                    candidate.ownerName == tr->name && candidate.name != "self")
                    ++methods;
            }
            if (owner != nullptr) {
                for (const auto &artifact : owner->modules()) {
                    if (artifact->frontend == nullptr || artifact->key == module)
                        continue;
                    for (const auto &candidate : artifact->frontend->declarations()) {
                        if (candidate.kind == frontend::DeclKind::Function &&
                            candidate.ownerName == tr->name)
                            ++methods;
                    }
                }
            }
        }
        if (methods == 0) {
            report(type.span,
                   is_iface ? "'dyn Interface' requires at least one method requirement"
                            : "'dyn Trait' requires at least one method requirement",
                   diagnostics::err::TypeMismatch);
            return error_type;
        }
        return type_table.internDyn(target, methods);
    }
    case frontend::TypeExprKind::Error:
        return kInvalidTypeId;
    }
    return kInvalidTypeId;
}

TypeId PerModuleSema::methodSelfParamType(const frontend::Parameter &param) {
    const TypeId declared = lowerTypeExpr(param.type);
    if (!declared)
        return declared;
    const auto *qualifier = type_table.qualified(type_table.canonical(declared));
    if (qualifier == nullptr || (qualifier->ownership != types::OwnershipKind::Lend &&
                                 qualifier->ownership != types::OwnershipKind::View)) {
        return declared;
    }
    // `self: lend Owner` / `self: view Owner` is a borrow receiver: the
    // method body receives a pointer to Owner, and the qualifier remains on
    // the pointee so ownership/read-only checks still recognize it.
    const TypeId inner = type_table.stripQualifiers(declared);
    if (!inner || type_table.kindOf(resolve(inner)) != TypeKind::Struct)
        return declared;
    return type_table.internPointer(
        type_table.internQualified(inner, qualifier->ownership, qualifier->isMut));
}

TypeId PerModuleSema::borrowParamType(const frontend::Parameter &param) {
    const TypeId declared = lowerTypeExpr(param.type);
    if (!declared)
        return declared;
    const auto *qualifier = type_table.qualified(type_table.canonical(declared));
    if (qualifier == nullptr || (qualifier->ownership != types::OwnershipKind::Lend &&
                                 qualifier->ownership != types::OwnershipKind::View)) {
        return declared;
    }
    const TypeId inner = type_table.stripQualifiers(declared);
    if (!inner)
        return declared;
    return type_table.internPointer(
        type_table.internQualified(inner, qualifier->ownership, qualifier->isMut));
}

bool PerModuleSema::isSelfReceiver(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto *resolved = findResolvedExpr(id);
    if (resolved == nullptr || !resolved->local)
        return false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || decl.ownerName.empty() ||
            decl.parameters.empty() || decl.parameters.front().id != resolved->local)
            continue;
        return decl.parameters.front().name == "self";
    }
    return false;
}

bool PerModuleSema::isBorrowParameter(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto *resolved = findResolvedExpr(id);
    if (resolved == nullptr || !resolved->local)
        return false;
    const TypeId param_type = typeOfLocal(resolved->local);
    return isBorrowParamType(param_type);
}

bool PerModuleSema::isBorrowParamType(TypeId type) const noexcept {
    if (!type)
        return false;
    // The ABI type is a pointer whose pointee is qualified. Iterating a
    // transparent alias or nominal preserves spelling while still recognizing
    // `*lend T` / `*view T` when a borrow type flows through one.
    TypeId current = type;
    for (unsigned guard = 0; guard < 8U; ++guard) {
        current = type_table.canonical(current);
        if (const auto *qualified = type_table.qualified(current); qualified != nullptr) {
            current = qualified->inner;
            continue;
        }
        break;
    }
    const auto *pointer = type_table.pointer(current);
    if (pointer == nullptr)
        return false;
    const auto *qualified = type_table.qualified(type_table.canonical(pointer->pointee));
    return qualified != nullptr && (qualified->ownership == types::OwnershipKind::Lend ||
                                    qualified->ownership == types::OwnershipKind::View);
}

bool PerModuleSema::checkOwnershipCoercion(
    frontend::ExprId arg, TypeId param_type,
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> &seen_roots,
    frontend::TextSpan call_span, bool report_error) {
    if (!arg || !param_type || arg.value > snapshot.expressions().size())
        return true;
    if (!isBorrowParamType(param_type))
        return true;

    const auto &annotated = snapshot.expressions()[arg.value - 1U];
    const bool has_annotation =
        annotated.kind == frontend::ExprKind::OwnershipCoerce && !annotated.operands.empty();
    const frontend::ExprId inner = has_annotation ? annotated.operands[0] : arg;
    if (!inner || inner.value > snapshot.expressions().size())
        return true;

    TypeId target = param_type;
    for (unsigned guard = 0; guard < 8U; ++guard) {
        target = type_table.canonical(target);
        if (const auto *qualified = type_table.qualified(target); qualified != nullptr) {
            target = qualified->inner;
            continue;
        }
        break;
    }
    const auto *pointer = type_table.pointer(target);
    if (pointer == nullptr)
        return true;
    const auto *pointee_qual = type_table.qualified(type_table.canonical(pointer->pointee));
    if (pointee_qual == nullptr || (pointee_qual->ownership != types::OwnershipKind::Lend &&
                                    pointee_qual->ownership != types::OwnershipKind::View))
        return true;
    const types::OwnershipKind target_ownership = pointee_qual->ownership;

    const auto &inner_expr                = snapshot.expressions()[inner.value - 1U];
    types::OwnershipKind source_ownership = types::OwnershipKind::Default;
    if (has_annotation) {
        source_ownership = mapOwnership(annotated.ownership);
    } else if (const TypeId source_type = typeOfExpr(inner); source_type) {
        if (const auto *initial_qualifier = type_table.qualified(type_table.canonical(source_type));
            initial_qualifier != nullptr &&
            initial_qualifier->ownership != types::OwnershipKind::Default)
            source_ownership = initial_qualifier->ownership;
        TypeId source_cursor = type_table.canonical(source_type);
        for (unsigned guard = 0; guard < 8U; ++guard) {
            if (const auto *source_qualifier = type_table.qualified(source_cursor);
                source_qualifier != nullptr) {
                source_cursor = source_qualifier->inner;
                continue;
            }
            break;
        }
        if (const auto *source_ptr = type_table.pointer(source_cursor); source_ptr != nullptr) {
            const auto *source_qual =
                type_table.qualified(type_table.canonical(source_ptr->pointee));
            if (source_qual != nullptr)
                source_ownership = source_qual->ownership;
        } else {
            const auto *source_qual = type_table.qualified(source_cursor);
            if (source_qual != nullptr)
                source_ownership = source_qual->ownership;
        }
    }

    // Literals, call results and other rvalue/temporary expressions need no
    // call-site annotation even though they cannot be borrowed in place. The
    // semantics is "materialize a temporary where the ABI needs an address".
    const bool place_expression = inner_expr.kind == frontend::ExprKind::Name ||
                                  inner_expr.kind == frontend::ExprKind::Field ||
                                  inner_expr.kind == frontend::ExprKind::Arrow ||
                                  inner_expr.kind == frontend::ExprKind::Index;
    const bool direct_binding = inner_expr.kind == frontend::ExprKind::Name;
    const bool already_borrow = source_ownership == types::OwnershipKind::Lend ||
                                source_ownership == types::OwnershipKind::View;
    const bool annotation_missing = !has_annotation && direct_binding && !already_borrow;

    if (has_annotation && source_ownership != target_ownership) {
        if (report_error) {
            const std::string required =
                target_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            const std::string written =
                source_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            report(annotated.span,
                   "call argument annotation mismatch: parameter expects '" + required +
                       "', call site wrote '" + written + "'",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }
    if (annotation_missing) {
        if (report_error) {
            const std::string required =
                target_ownership == types::OwnershipKind::Lend ? "lend" : "view";
            report(inner_expr.span,
                   "call to '" + std::string(inner_expr.text) + "' needs an explicit '" + required +
                       "' annotation for this borrow parameter",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }

    if (!has_annotation || !place_expression)
        return true;
    // Plain bindings conflict by the local they resolve to, so a call can
    // never lend the same variable twice. Field/index/arrow paths use the full
    // place id: two identical field paths conflict, but distinct fields and
    // distinct receiver roots remain separate borrows in this slice.
    frontend::ExprId conflict_root;
    if (direct_binding) {
        const auto *inner_resolved = findResolvedExpr(inner);
        conflict_root              = inner_resolved != nullptr && inner_resolved->local
                                         ? frontend::ExprId{inner_resolved->local.value}
                                         : inner;
    } else {
        conflict_root = inner;
    }
    if (!conflict_root)
        return true;
    const auto conflict_kind = source_ownership == types::OwnershipKind::Lend
                                   ? types::OwnershipKind::Lend
                                   : types::OwnershipKind::View;
    for (const auto &seen : seen_roots) {
        if (seen.first != conflict_root)
            continue;
        const bool lend_conflict = conflict_kind == types::OwnershipKind::Lend &&
                                   seen.second == types::OwnershipKind::Lend;
        const bool mixed_conflict = (conflict_kind == types::OwnershipKind::Lend &&
                                     seen.second == types::OwnershipKind::View) ||
                                    (conflict_kind == types::OwnershipKind::View &&
                                     seen.second == types::OwnershipKind::Lend);
        if (!lend_conflict && !mixed_conflict)
            continue;
        if (report_error) {
            report(call_span, "the same binding cannot be borrowed more than once in this call",
                   diagnostics::err::OwnershipCoercionRequired);
        }
        return false;
    }
    seen_roots.emplace_back(conflict_root, conflict_kind);
    return true;
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

TypeId PerModuleSema::lowerForeignConstantType(const cinterop::Constant &constant) {
    switch (constant.kind) {
    case cinterop::ConstantKind::Integer:
        return type_table.internInteger({constant.bits, constant.isSigned});
    case cinterop::ConstantKind::Float:
        return type_table.internFloat({constant.bits});
    case cinterop::ConstantKind::Bool:
        return bool_type;
    case cinterop::ConstantKind::Char:
        return char_type;
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
        auto &field_meta                         = type_table.makeFieldMetaStorage();
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
        for (const auto &param : template_decl->parameters) {
            TypeId ftype = lowerTypeExpr(param.type);
            fields.push(ftype ? ftype : error_type);
            field_meta.push(FieldMeta{param.visibility, param.modDepth, module});
            char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
            std::memcpy(buf, param.name.data(), param.name.size());
            fld_names.push(std::string_view(buf, param.name.size()));
        }
        activeTemplateArgs_ = std::move(saved_active);
        TypeId st = type_table.internStruct(concrete_name, fields, &fld_names, &field_meta);
        type_table.registerNamed(concrete_name, st);
        return st;
    }
    case frontend::DeclKind::TypeAlias: {
        std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
        activeTemplateArgs_.clear();
        for (size_t i = 0; i < template_decl->genericParams.size(); ++i) {
            GenericBinding active_binding;
            active_binding.name = template_decl->genericParams[i].name;
            active_binding.type = args[i];
            activeTemplateArgs_.push_back(std::move(active_binding));
        }
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
    auto &field_meta                               = type_table.makeFieldMetaStorage();
    const std::vector<GenericBinding> saved_active = std::move(activeTemplateArgs_);
    activeTemplateArgs_.clear();
    for (size_t i = 0; i < template_decl.genericParams.size(); ++i) {
        GenericBinding active_binding;
        active_binding.name = template_decl.genericParams[i].name;
        active_binding.type = args[i];
        activeTemplateArgs_.push_back(std::move(active_binding));
    }
    for (const auto &param : template_decl.parameters) {
        const TypeId field_type = lowerTypeExpr(param.type);
        fields.push(field_type ? field_type : error_type);
        field_meta.push(FieldMeta{param.visibility, param.modDepth, module});
        char *buf = static_cast<char *>(arena.alloc(param.name.size(), 1));
        std::memcpy(buf, param.name.data(), param.name.size());
        field_names.push(std::string_view(buf, param.name.size()));
    }
    activeTemplateArgs_ = saved_active;

    const TypeId st = type_table.internStruct(concrete_name, fields, &field_names, &field_meta);
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
    if (expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(id);
            resolved != nullptr && resolved->local &&
            movedLocals_.contains(resolved->local.value)) {
            report(expr.span,
                   "cannot use '" + expr.text + "' after it was moved by a previous call",
                   diagnostics::err::UseAfterMove);
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
    if (binding.foreignConstant != nullptr)
        return lowerForeignConstantType(*binding.foreignConstant);
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
                              std::vector<OverloadCandidate> &candidates, size_t implicit_args,
                              bool &reported) {
    reported                  = false;
    const size_t written_args = call.operands.size() - 1U;
    std::vector<const OverloadCandidate *> viable;
    std::vector<const OverloadCandidate *> exact_matches;
    bool widened_pointer = false;
    for (auto &candidate : candidates) {
        if (candidate.fn == nullptr)
            continue;
        const bool slice_candidate = candidate.binding != nullptr &&
                                     candidate.binding->isVariadicSlice &&
                                     candidate.fn != nullptr && !candidate.fn->params.empty();
        candidate.variadicSlice = candidate.binding != nullptr &&
                                  candidate.binding->isVariadicSlice && candidate.fn != nullptr &&
                                  !candidate.fn->params.empty();
        const size_t fixed_params = candidate.fn->params.size() - (slice_candidate ? 1U : 0U);
        if (slice_candidate) {
            if (written_args < fixed_params)
                continue;
        } else if (candidate.fn->params.size() != written_args + implicit_args) {
            continue;
        }
        bool fits                 = true;
        bool exact                = true;
        bool widens_ptr           = false;
        const size_t probe_params = slice_candidate ? fixed_params : written_args;
        for (size_t index = 0; index < probe_params && fits; ++index) {
            const frontend::ExprId arg = call.operands[index + 1U];
            const TypeId arg_type      = inferExpr(arg);
            const size_t param_index   = index + implicit_args;
            const TypeId param_type    = candidate.fn->params[param_index];
            // Probe without mutating the recorded type: a rejected candidate must
            // not leave a literal retyped for a signature that was not chosen.
            std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
            const bool borrow_ok =
                checkOwnershipCoercion(arg, param_type, seen_roots, call.span, false);
            const bool type_fits =
                isBorrowParamType(param_type)
                    ? coerceValue(arg, param_type, arg_type)
                    : (coercesTo(param_type, arg_type) || literalAdaptsTo(arg, param_type));
            fits            = type_fits && borrow_ok;
            const bool same = sameType(param_type, arg_type);
            exact           = exact && same;
            widens_ptr      = widens_ptr || (!same && isVoidPointer(param_type) &&
                                        static_cast<bool>(pointerBase(arg_type)));
        }
        if (slice_candidate) {
            // Validate the auto-collected tail against the slice element. An
            // explicit `[]T` argument is a normal fixed-arity call and is left
            // to the caller's regular coercion loop. Numeric literals also
            // adapt here so `f(1, 2)` can match `f(xs: [...]i32)`.
            const auto *slice = type_table.slice(candidate.fn->params.back());
            if (slice != nullptr) {
                for (size_t index = fixed_params; index < written_args && fits; ++index) {
                    const frontend::ExprId arg = call.operands[index + 1U];
                    const TypeId arg_type      = inferExpr(arg);
                    fits =
                        coercesTo(slice->element, arg_type) || literalAdaptsTo(arg, slice->element);
                    if (fits)
                        exact = false;
                }
            } else {
                fits = false;
            }
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
    // A fixed-arity overload that matches exactly is preferred over a
    // variadic-slice overload even when the latter also fits. This is the
    // explicit design contract for adding `[...]T` tails beside existing `fn`s.
    const bool has_fixed_exact =
        std::any_of(exact_matches.begin(), exact_matches.end(),
                    [](const OverloadCandidate *c) { return c != nullptr && !c->variadicSlice; });
    if (has_fixed_exact) {
        viable.erase(std::remove_if(viable.begin(), viable.end(),
                                    [](const OverloadCandidate *c) {
                                        return c != nullptr && c->variadicSlice;
                                    }),
                     viable.end());
        exact_matches.erase(std::remove_if(exact_matches.begin(), exact_matches.end(),
                                           [](const OverloadCandidate *c) {
                                               return c != nullptr && c->variadicSlice;
                                           }),
                            exact_matches.end());
    }
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
                candidate.module = binding->target.module.empty() ? module : binding->target.module;
                candidate.decl   = binding->declaration;
                if (candidate.fn != nullptr && !typeContainsGeneric(candidate.fn))
                    candidates.push_back(candidate);
            }
            if (candidates.size() > 1U) {
                bool reported = false;
                if (const auto *chosen = selectOverload(expr, candidates, 0U, reported)) {
                    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
                    const size_t probe_params = chosen->variadicSlice
                                                    ? chosen->fn->params.size() - 1U
                                                    : chosen->fn->params.size();
                    for (size_t index = 0;
                         index < probe_params && index < expr.operands.size() - 1U; ++index) {
                        const TypeId arg_type = inferExpr(expr.operands[index + 1U]);
                        (void)checkOwnershipCoercion(expr.operands[index + 1U],
                                                     chosen->fn->params[index], seen_roots,
                                                     expr.span, true);
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
    const bool is_variadic_slice =
        resolved_callee != nullptr && bindingIsVariadicSlice(*resolved_callee);
    TypeId callee_type = inferExpr(expr.operands[0]);
    const auto *fn     = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function", diagnostics::err::NoMatchingFn);
        return error_type;
    }
    size_t arg_count       = expr.operands.size() - 1;
    size_t fixed_arg_count = is_variadic ? fn->params.size() : 0;
    size_t slice_index =
        is_variadic_slice ? variadicSliceParam(resolved_callee, fn) : fn->params.size();
    const bool explicit_slice_arg = [&]() {
        if (!is_variadic_slice || arg_count != slice_index + 1U ||
            slice_index + 1U >= expr.operands.size())
            return false;
        const TypeId last_arg = inferExpr(expr.operands[slice_index + 1U]);
        const TypeId last     = resolve(last_arg);
        return type_table.slice(last) != nullptr || type_table.array(last) != nullptr;
    }();
    const bool auto_collected_tail =
        is_variadic_slice &&
        (arg_count > slice_index + 1U || (arg_count == slice_index + 1U && !explicit_slice_arg));
    if (is_variadic_slice) {
        // The slice parameter is not auto-collected when the caller passes an
        // explicit slice as the final argument. That keeps `fn f(xs: [...]T)`
        // callable with an existing `[]T` value as well as with bare elements.
        if (slice_index >= fn->params.size()) {
            report(expr.span, "variadic slice function has no slice parameter",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
        if (arg_count < slice_index) {
            report(expr.span, "variadic slice function call has too few arguments",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
    } else if (is_variadic) {
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
        for (size_t index = 0; index < fn->params.size(); ++index) {
            if (is_variadic_slice && index == slice_index) {
                // If the caller passes a single explicit slice value, keep it
                // so `f([]T)` still instantiates `T`; otherwise infer `[]T`
                // from the first auto-collected element.
                if (arg_count == slice_index + 1U && index + 1U < expr.operands.size()) {
                    const TypeId last_arg =
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]);
                    if (const auto *arg_slice = type_table.slice(resolve(last_arg))) {
                        (void)arg_slice;
                        argument_types.push_back(last_arg);
                    } else {
                        argument_types.push_back(type_table.internSlice(last_arg));
                    }
                } else if (arg_count > slice_index + 1U && index + 1U < expr.operands.size()) {
                    const TypeId element_sample =
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]);
                    argument_types.push_back(type_table.internSlice(element_sample));
                } else {
                    argument_types.push_back(kInvalidTypeId);
                }
            } else if (index + 1U < expr.operands.size()) {
                argument_types.push_back(inferExpr(expr.operands[static_cast<size_t>(index + 1U)]));
            }
        }

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
        if (generic_decl != nullptr)
            checkGenericConstraints(*generic_decl, args, expr.span);

        const size_t instance_index =
            instantiations->bindCall(module, callee.id, target_module, decl_id, args);
        if (instance_index == ~size_t{0}) {
            report(expr.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        }
        const TypeId instance_type = instantiations->substituteFunction(*fn, args);
        const auto *instance_fn    = type_table.function(instance_type);
        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
        const size_t checked_params = is_variadic_slice ? slice_index : fn->params.size();
        for (size_t index = 0;
             index < checked_params && instance_fn != nullptr && index < instance_fn->params.size();
             ++index) {
            TypeId arg_type = argument_types[index];
            (void)checkOwnershipCoercion(expr.operands[index + 1U], instance_fn->params[index],
                                         seen_roots, expr.span, true);
            if (!coerceValue(expr.operands[index + 1U], instance_fn->params[index], arg_type))
                reportCoercionFailure(expr.span, instance_fn->params[index], arg_type,
                                      "generic function call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
        if (auto_collected_tail && instance_fn != nullptr) {
            (void)checkVariadicTail(expr.span, expr.operands, instance_fn, slice_index, true);
        }
        setExprType(callee.id, instance_type);
        setResolvedCallTarget(callee.id, target_module, decl_id);
        return instance_fn != nullptr ? instance_fn->result : error_type;
    }

    const size_t checked_params = is_variadic_slice
                                      ? (explicit_slice_arg ? slice_index + 1U : slice_index)
                                  : is_variadic ? std::min(fixed_arg_count, fn->params.size())
                                                : fn->params.size();
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t i = 0; i < checked_params; ++i) {
        TypeId arg_type = inferExpr(expr.operands[i + 1]);
        (void)checkOwnershipCoercion(expr.operands[i + 1], fn->params[i], seen_roots, expr.span,
                                     true);
        if (!coerceValue(expr.operands[i + 1], fn->params[i], arg_type))
            reportCoercionFailure(expr.span, fn->params[i], arg_type,
                                  "function call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    if (auto_collected_tail) {
        (void)checkVariadicTail(expr.span, expr.operands, fn, slice_index, true);
    }
    // The trailing variadic arguments have no declared Zith type: infer them
    // (for diagnostics) but do not require a conversion or a fixed arity.
    for (size_t i = std::max<size_t>(1, checked_params + 1); i < expr.operands.size(); ++i)
        (void)inferExpr(expr.operands[i]);
    return fn->result;
}

std::vector<PerModuleSema::ResolvedMethod>
PerModuleSema::findMethodsForOwner(std::string_view owner_name,
                                   std::string_view method_name) const {
    std::vector<ResolvedMethod> methods;
    const auto matches = [&](const frontend::Declaration &decl) {
        return decl.kind == frontend::DeclKind::Function && decl.ownerName == owner_name &&
               decl.name == method_name;
    };

    // Keep the existing local-first behavior: a method declared in the current
    // module must continue to take precedence over same-named imports.
    for (const auto &decl : snapshot.declarations()) {
        if (matches(decl)) {
            const bool is_trait_method = !decl.ownerName.empty() && !decl.traitName.empty() &&
                                         decl.ownerName == decl.traitName;
            methods.push_back(ResolvedMethod{module, &decl, decl.traitName, is_trait_method});
        }
    }
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.key == module || artifact.frontend == nullptr)
                continue;
            for (const auto &decl : artifact.frontend->declarations()) {
                if (matches(decl)) {
                    const bool is_trait_method = !decl.ownerName.empty() &&
                                                 !decl.traitName.empty() &&
                                                 decl.ownerName == decl.traitName;
                    methods.push_back(
                        ResolvedMethod{artifact.key, &decl, decl.traitName, is_trait_method});
                }
            }
        }
    }

    // Trait defaults are exposed on every owner that satisfies their trait.
    // They are collected after owner methods so local/impl methods dominate
    // method-name overload selection.
    const TypeId owner_type     = type_table.lookupNamed(owner_name);
    const auto addTraitDefaults = [&](const frontend::FrontendSnapshot &snap,
                                      session::ModuleKey snap_module) {
        for (const auto &decl : snap.declarations()) {
            if (decl.kind != frontend::DeclKind::Function || !decl.body || decl.ownerName.empty() ||
                decl.traitName != decl.ownerName || decl.name != method_name)
                continue;
            const TypeId trait_type = type_table.lookupNamed(decl.traitName);
            if (!trait_type || !satisfiesConformance(owner_type, trait_type))
                continue;
            methods.push_back(ResolvedMethod{snap_module, &decl, decl.traitName, true});
        }
    };
    addTraitDefaults(snapshot, module);
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            addTraitDefaults(*artifact.frontend, artifact.key);
        }
    }
    return methods;
}

const frontend::Declaration *PerModuleSema::findDeclNamed(std::string_view name,
                                                          frontend::DeclKind kind) const {
    const auto findIn =
        [&](const frontend::FrontendSnapshot &snap) -> const frontend::Declaration * {
        for (const auto &decl : snap.declarations()) {
            if (decl.kind == kind && decl.name == name)
                return &decl;
        }
        return nullptr;
    };
    if (const auto *found = findIn(snapshot))
        return found;
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            if (const auto *found = findIn(*artifact.frontend))
                return found;
        }
    }
    return nullptr;
}

bool PerModuleSema::isInterfaceType(TypeId type) const {
    const TypeId resolved = resolve(type);
    const auto *trait_ty  = type_table.trait(resolved);
    return trait_ty != nullptr &&
           findDeclNamed(trait_ty->name, frontend::DeclKind::Interface) != nullptr;
}

TypeId PerModuleSema::lowerTypeExprConst(frontend::TypeExprId id) const {
    if (!id || id.value > snapshot.typeExpressions().size())
        return kInvalidTypeId;
    const auto &type = snapshot.typeExpressions()[id.value - 1U];
    if (type.ownership != frontend::OwnershipKind::Default || type.isMut) {
        frontend::TypeExpression bare = type;
        bare.ownership                = frontend::OwnershipKind::Default;
        bare.isMut                    = false;
        bare.hasMutKeyword            = false;
        // Existing type lowering mutates generic/instantiation state on some
        // paths; the const helper is only used for already-lowered interface
        // field introspection, so reentering it safely is unnecessary.
        (void)bare;
    }
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Interface && decl.name == type.name)
            return type_table.lookupNamed(decl.name);
    }
    return type_table.lookupNamed(type.name);
}

bool PerModuleSema::satisfiesConformance(TypeId type, TypeId trait_or_interface) const {
    const TypeId concrete = resolve(type);
    const TypeId target   = resolve(trait_or_interface);
    if (!concrete || !target)
        return false;
    if (type_table.conformanceTable().satisfies(concrete, target))
        return true;

    // Interfaces are structural: compare the declared fields and methods of the
    // interface against the members of the concrete type. The interface must be
    // a named Interface declaration; its `parameters` are the field list and
    // its method requirements are Function declarations with `ownerName` set.
    if (type_table.kindOf(target) != TypeKind::Trait)
        return false;
    const auto *interface_ty = type_table.trait(target);
    if (interface_ty == nullptr || !isInterfaceType(target))
        return false;

    const frontend::Declaration *interface_decl =
        findDeclNamed(interface_ty->name, frontend::DeclKind::Interface);
    if (interface_decl == nullptr)
        return false;

    const auto *struct_type = type_table.struct_type(concrete);
    const auto *nominal     = type_table.nominal(concrete);
    if (struct_type == nullptr && nominal == nullptr)
        return false;

    // Generic/interface-only types cannot be checked structurally.
    if (struct_type == nullptr)
        return false;

    for (const auto &required : interface_decl->parameters) {
        const int index = type_table.fieldIndex(concrete, required.name);
        if (index < 0)
            return false;
        const TypeId required_type = lowerTypeExprConst(required.type);
        const TypeId actual_type   = index < static_cast<int>(struct_type->fields.size())
                                         ? struct_type->fields[static_cast<size_t>(index)]
                                         : kInvalidTypeId;
        if (required_type && actual_type && !sameType(required_type, actual_type))
            return false;
    }
    const auto checkMethod = [&](const frontend::Declaration &requirement,
                                 const frontend::Declaration &candidate,
                                 session::ModuleKey requirement_module,
                                 session::ModuleKey candidate_module) {
        if (requirement.parameters.size() != candidate.parameters.size())
            return false;
        const auto req_sema =
            owner != nullptr ? owner->findModuleSema(requirement_module) : nullptr;
        const auto cand_sema = owner != nullptr ? owner->findModuleSema(candidate_module) : nullptr;
        const auto *req_type =
            type_table.function(req_sema != nullptr ? req_sema->typeOfDecl(requirement.id)
                                                    : typeOfDecl(requirement.id));
        const auto *cand_type = type_table.function(
            cand_sema != nullptr ? cand_sema->typeOfDecl(candidate.id) : typeOfDecl(candidate.id));
        if (req_type == nullptr || cand_type == nullptr ||
            req_type->params.size() != cand_type->params.size())
            return false;
        if (!sameType(substituteSelf(cand_type->result, concrete, target),
                      substituteSelf(req_type->result, concrete, target)))
            return false;
        for (size_t index = 0; index < req_type->params.size(); ++index) {
            const TypeId expected = substituteSelf(req_type->params[index], concrete, target);
            if (expected && !sameType(expected, cand_type->params[index]))
                return false;
        }
        return true;
    };
    const auto checkRequirements = [&](const frontend::FrontendSnapshot &snap,
                                       session::ModuleKey snap_module) {
        for (const auto &requirement : snap.declarations()) {
            if (requirement.kind != frontend::DeclKind::Function ||
                requirement.ownerName != interface_ty->name || requirement.body)
                continue;
            const auto methods =
                findMethodsForOwner(std::string_view{struct_type->name}, requirement.name);
            bool matched = false;
            for (const auto &method : methods) {
                if (method.isTraitMethod)
                    continue;
                if (checkMethod(requirement, *method.decl, snap_module, method.module)) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        return true;
    };
    if (!checkRequirements(snapshot, module))
        return false;
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.key == module || artifact.frontend == nullptr)
                continue;
            if (!checkRequirements(*artifact.frontend, artifact.key))
                return false;
        }
    }
    return true;
}

void PerModuleSema::checkGenericConstraints(const frontend::Declaration &generic_decl,
                                            const std::vector<TypeId> &args,
                                            frontend::TextSpan span) {
    const auto found = genericParams_.find(generic_decl.id.value);
    if (found == genericParams_.end())
        return;
    for (size_t index = 0; index < args.size() && index < found->second.size(); ++index) {
        for (const TypeId bound : found->second[index].bounds) {
            if (!satisfiesConformance(args[index], bound)) {
                const diagnostics::ErrCode code = isInterfaceType(bound)
                                                      ? diagnostics::err::InterfaceNotSatisfied
                                                      : diagnostics::err::ConstraintNotSatisfied;
                report(span,
                       "type '" + type_table.typeToString(args[index]) +
                           "' does not satisfy constraint '" + type_table.typeToString(bound) + "'",
                       code);
            }
        }
    }
}

std::vector<TypeId> PerModuleSema::boundsForGenericParam(TypeId generic_type) const {
    uint32_t decl_id   = 0;
    uint32_t param_idx = 0;
    type_table.genericParamOrigin(generic_type, &decl_id, &param_idx);
    const auto found = genericParams_.find(decl_id);
    if (found == genericParams_.end() || param_idx >= found->second.size())
        return {};
    return found->second[param_idx].bounds;
}

/// Try to resolve `expr` (a Call whose callee is a Field/Arrow) as a
/// method call on the base type. Returns the result TypeId on success,
/// or `kInvalidTypeId` (with a diagnostic already reported) when the
/// field is not a method.
TypeId PerModuleSema::inferMethodCall(const frontend::Expression &call,
                                      const frontend::Expression &callee) {
    // `p.Trait.method()` is parsed as `Call(Field(Field(p, Trait), method))`.
    // The intermediate `p.Trait` is not a real field; it names a trait or
    // interface that the receiver type satisfies, so resolve the method within
    // that trait before falling back to the ordinary method lookup.
    frontend::ExprId receiver_id = callee.operands[0];
    std::string qualifying_trait;
    if (receiver_id && receiver_id.value <= snapshot.expressions().size()) {
        const auto &outer = snapshot.expressions()[receiver_id.value - 1U];
        if ((outer.kind == frontend::ExprKind::Field || outer.kind == frontend::ExprKind::Arrow) &&
            !outer.operands.empty()) {
            const TypeId owner_base = inferExpr(outer.operands[0]);
            const TypeId pointee    = resolve(owner_base);
            if (pointee && type_table.kindOf(pointee) == TypeKind::Struct) {
                const TypeId trait_type = type_table.lookupNamed(outer.text);
                if (trait_type && type_table.kindOf(trait_type) == TypeKind::Trait &&
                    satisfiesConformance(pointee, trait_type)) {
                    qualifying_trait = outer.text;
                    typed_map.traitQualifiedReceiverBase.insert(receiver_id.value,
                                                                outer.operands[0].value);
                    setExprType(receiver_id, pointee);
                    receiver_id = outer.operands[0];
                }
            }
        }
    }

    const TypeId base_type = inferExpr(receiver_id);
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
        if (const auto *opt = type_table.optional(pointee)) {
            pointee = resolve(opt->inner);
            // C pointers are modeled as `?*T`. While the optional wrapper is
            // still present in the expression/lvalue type, method calls pass
            // the pointer value itself as the receiver argument.
            if (const auto *ptr = type_table.pointer(pointee)) {
                pointee    = resolve(ptr->pointee);
                is_pointer = true;
            }
        }
    }

    const TypeId resolved_base = resolve(base_type);
    if (type_table.kindOf(resolved_base) == TypeKind::Dyn)
        return inferDynMethodCall(call, callee, resolved_base);

    const StructType *st = type_table.struct_type(pointee);
    if (st == nullptr && type_table.kindOf(pointee) == TypeKind::GenericParam) {
        // A generic parameter is only callable as `T.method()` when one of its
        // bounds is a trait whose method set contains `method`. Substitution of
        // `Self` makes the trait signature usable for `T`.
        const std::vector<TypeId> bounds = boundsForGenericParam(pointee);
        std::vector<const frontend::Declaration *> bound_decls;
        std::vector<session::ModuleKey> bound_modules;
        std::vector<TypeId> bound_traits;
        for (const TypeId bound : bounds) {
            const auto *trait_ty = type_table.trait(resolve(bound));
            if (trait_ty == nullptr)
                continue;
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.ownerName != trait_ty->name ||
                    decl.name != callee.text)
                    continue;
                bound_decls.push_back(&decl);
                bound_modules.push_back(module);
                bound_traits.push_back(bound);
                break;
            }
            if (owner != nullptr) {
                for (const auto &artifact_ptr : owner->modules()) {
                    const auto &artifact = *artifact_ptr;
                    if (artifact.frontend == nullptr || artifact.key == module)
                        continue;
                    bool found = false;
                    for (const auto &decl : artifact.frontend->declarations()) {
                        if (decl.kind != frontend::DeclKind::Function ||
                            decl.ownerName != trait_ty->name || decl.name != callee.text)
                            continue;
                        bound_decls.push_back(&decl);
                        bound_modules.push_back(artifact.key);
                        bound_traits.push_back(bound);
                        found = true;
                        break;
                    }
                    if (found)
                        break;
                }
            }
        }
        if (bound_decls.empty())
            return kInvalidTypeId;

        // A single declaration can be reachable from multiple bounds (for
        // example `T: A + B` where both bounds declare the same default
        // method). Only report ambiguity when distinct declarations exist.
        std::vector<const frontend::Declaration *> unique_decls;
        for (const auto *decl : bound_decls) {
            const bool seen = std::any_of(
                unique_decls.begin(), unique_decls.end(),
                [decl](const frontend::Declaration *existing) { return existing->id == decl->id; });
            if (!seen)
                unique_decls.push_back(decl);
        }
        if (unique_decls.size() != 1U) {
            report(call.span,
                   "method call is ambiguous on generic parameter '" +
                       type_table.typeToString(pointee) + "'",
                   diagnostics::err::AmbiguousCall);
            return error_type;
        }

        const frontend::Declaration *method_decl = bound_decls.front();
        const session::ModuleKey method_module   = bound_modules.front();
        PerModuleSema *method_sema =
            owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
        const TypeId fn_type = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                      : typeOfDecl(method_decl->id);
        const auto *fn       = type_table.function(fn_type);
        if (fn == nullptr)
            return kInvalidTypeId;

        const TypeId substituted = substituteSelf(fn_type, pointee, bound_traits.front());
        const auto *sub_fn       = type_table.function(substituted);
        if (sub_fn == nullptr)
            return kInvalidTypeId;

        const bool has_receiver =
            !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
        const size_t provided_args = call.operands.size() - 1U;
        const bool target_is_slice =
            !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
        const size_t slice_param_index =
            target_is_slice ? sub_fn->params.size() - 1U : sub_fn->params.size();
        const size_t fixed_explicit_args = target_is_slice
                                               ? slice_param_index - (has_receiver ? 1U : 0U)
                                               : sub_fn->params.size() - (has_receiver ? 1U : 0U);
        const bool explicit_slice_arg    = [&]() {
            if (!target_is_slice || provided_args != fixed_explicit_args + 1U ||
                call.operands.empty())
                return false;
            const TypeId last = resolve(inferExpr(call.operands.back()));
            return type_table.slice(last) != nullptr || type_table.array(last) != nullptr;
        }();
        const bool auto_collected_tail = target_is_slice && !explicit_slice_arg;
        if (provided_args < fixed_explicit_args ||
            (!target_is_slice && provided_args != fixed_explicit_args) ||
            (target_is_slice && !auto_collected_tail && !explicit_slice_arg &&
             provided_args != fixed_explicit_args)) {
            report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
            return error_type;
        }
        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
        for (size_t explicit_index = 0U; explicit_index < fixed_explicit_args; ++explicit_index) {
            const size_t param_index = has_receiver ? explicit_index + 1U : explicit_index;
            const size_t arg_index   = explicit_index + 1U;
            if (arg_index < call.operands.size()) {
                const TypeId arg_type = inferExpr(call.operands[arg_index]);
                (void)checkOwnershipCoercion(call.operands[arg_index], sub_fn->params[param_index],
                                             seen_roots, call.span, true);
                if (!coerceValue(call.operands[arg_index], sub_fn->params[param_index], arg_type))
                    reportCoercionFailure(call.span, sub_fn->params[param_index], arg_type,
                                          "method call argument type mismatch",
                                          diagnostics::err::NoMatchingFn);
            }
        }
        if (auto_collected_tail && target_is_slice)
            (void)checkVariadicTailArgs(call.span, call.operands, sub_fn->params[slice_param_index],
                                        fixed_explicit_args + 1U, true);
        setExprType(callee.id, substituted);
        setResolvedCallTarget(callee.id, method_module, method_decl->id);
        return sub_fn->result;
    }
    if (st == nullptr)
        return kInvalidTypeId; // not a struct receiver: let normal call resolution run

    if (qualifying_trait.empty())
        return resolveStructMethodCall(call, callee,
                                       findMethodsForOwner(ownerNameOf(pointee), callee.text),
                                       base_type, pointee, is_pointer);

    // `p.Trait.method()` selects only declarations owned by that trait. For a
    // declaration-only requirement, the concrete owner method that satisfies it
    // is the callable target; an explicit impl method has the trait name and
    // must be resolved through the trait too.
    const std::string owner_name = ownerNameOf(pointee);
    const std::string trait_name = [&]() {
        std::string name = qualifying_trait;
        if (const size_t angle = name.find('<'); angle != std::string::npos)
            name.resize(angle);
        return name;
    }();
    const TypeId trait_type     = type_table.lookupNamed(trait_name);
    const bool interface_target = trait_type && isInterfaceType(trait_type);
    std::vector<ResolvedMethod> qualified;
    for (const auto &method : findMethodsForOwner(owner_name, callee.text)) {
        if (method.decl == nullptr)
            continue;
        const bool from_trait =
            !method.decl->traitName.empty() && method.decl->traitName == trait_name;
        const bool trait_requirement  = method.isTraitMethod && method.traitName == trait_name;
        const bool interface_concrete = interface_target && !method.isTraitMethod;
        if (from_trait || trait_requirement || interface_concrete)
            qualified.push_back(method);
    }
    if (qualified.empty() || !trait_type) {
        report(call.span,
               "type '" + type_table.typeToString(pointee) + "' has no member '" + callee.text +
                   "' through trait '" + trait_name + "'",
               diagnostics::err::NoMember);
        return error_type;
    }
    return resolveStructMethodCall(call, callee, qualified, base_type, pointee, is_pointer);
}

TypeId PerModuleSema::inferDynMethodCall(const frontend::Expression &call,
                                         const frontend::Expression &callee, TypeId dyn_type) {
    const auto *dyn = type_table.dyn_type(dyn_type);
    if (dyn == nullptr)
        return kInvalidTypeId;
    const TypeId target  = resolve(dyn->target);
    const auto *trait_ty = type_table.trait(target);
    if (trait_ty == nullptr)
        return kInvalidTypeId;

    const auto findMethod =
        [&](const frontend::FrontendSnapshot &snap, session::ModuleKey snap_module,
            const frontend::Declaration **out_decl, session::ModuleKey *out_module) {
            for (const auto &decl : snap.declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.ownerName != trait_ty->name ||
                    decl.name != callee.text)
                    continue;
                *out_decl   = &decl;
                *out_module = snap_module;
                return true;
            }
            return false;
        };

    const frontend::Declaration *method_decl = nullptr;
    session::ModuleKey method_module;
    if (findMethod(snapshot, module, &method_decl, &method_module)) {
        // already set below through the general local-first path.
    } else if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            if (findMethod(*artifact.frontend, artifact.key, &method_decl, &method_module))
                break;
        }
    }
    if (method_decl == nullptr) {
        report(call.span,
               "dyn type '" + type_table.typeToString(target) + "' has no method '" + callee.text +
                   "'",
               diagnostics::err::NoMember);
        return error_type;
    }

    PerModuleSema *method_sema = owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
    const TypeId method_type   = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                        : typeOfDecl(method_decl->id);
    const auto *fn             = type_table.function(method_type);
    if (fn == nullptr)
        return kInvalidTypeId;

    const bool has_receiver =
        !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
    const size_t provided_args = call.operands.size() - 1U;
    const bool target_is_slice =
        !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
    const size_t slice_param_index   = target_is_slice ? fn->params.size() - 1U : fn->params.size();
    const size_t fixed_explicit_args = target_is_slice
                                           ? slice_param_index - (has_receiver ? 1U : 0U)
                                           : fn->params.size() - (has_receiver ? 1U : 0U);
    const bool explicit_slice_arg    = [&]() {
        if (!target_is_slice || provided_args != fixed_explicit_args + 1U || call.operands.empty())
            return false;
        const TypeId last = resolve(inferExpr(call.operands.back()));
        return type_table.slice(last) != nullptr || type_table.array(last) != nullptr;
    }();
    const bool auto_collected_tail = target_is_slice && !explicit_slice_arg;
    if (provided_args < fixed_explicit_args ||
        (!target_is_slice && provided_args != fixed_explicit_args) ||
        (target_is_slice && !auto_collected_tail && !explicit_slice_arg &&
         provided_args != fixed_explicit_args)) {
        report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
        return error_type;
    }

    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t explicit_index = 0U; explicit_index < fixed_explicit_args; ++explicit_index) {
        const size_t param_index = has_receiver ? explicit_index + 1U : explicit_index;
        const size_t arg_index   = explicit_index + 1U;
        if (arg_index < call.operands.size()) {
            const TypeId arg_type = inferExpr(call.operands[arg_index]);
            (void)checkOwnershipCoercion(call.operands[arg_index], fn->params[param_index],
                                         seen_roots, call.span, true);
            if (!coerceValue(call.operands[arg_index], fn->params[param_index], arg_type))
                reportCoercionFailure(call.span, fn->params[param_index], arg_type,
                                      "method call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
    }
    if (auto_collected_tail && target_is_slice)
        (void)checkVariadicTailArgs(call.span, call.operands, fn->params[slice_param_index],
                                    fixed_explicit_args + 1U, true);

    setExprType(callee.id, method_type);
    setResolvedCallTarget(callee.id, method_module, method_decl->id);
    return fn->result;
}

std::string PerModuleSema::ownerNameOf(TypeId pointee) const {
    const auto *st = type_table.struct_type(pointee);
    if (st == nullptr)
        return {};
    std::string owner_name;
    owner_name = st->name;
    if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
        owner_name.resize(angle);
    return owner_name;
}

TypeId PerModuleSema::resolveStructMethodCall(const frontend::Expression &call,
                                              const frontend::Expression &callee,
                                              const std::vector<ResolvedMethod> &resolved_methods,
                                              TypeId base_type, TypeId pointee, bool is_pointer) {
    // Collect every method of this owner with the callee's name: methods take part
    // in the same overload resolution as free functions.
    std::string owner_name;
    if (const auto *st = type_table.struct_type(pointee)) {
        owner_name = st->name;
        if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
            owner_name.resize(angle);
    }
    std::vector<const frontend::Declaration *> method_decls;
    std::vector<session::ModuleKey> method_modules;
    std::vector<std::string> method_trait_names;
    std::vector<const frontend::Declaration *> default_decls;
    std::vector<session::ModuleKey> default_modules;
    std::vector<std::string> default_trait_names;
    for (const auto &method : resolved_methods) {
        const bool is_trait_decl = method.isTraitMethod;
        if (is_trait_decl) {
            const TypeId trait_type = type_table.lookupNamed(method.traitName);
            if (!trait_type || !satisfiesConformance(pointee, trait_type))
                continue;
            default_decls.push_back(method.decl);
            default_modules.push_back(method.module);
            default_trait_names.push_back(method.traitName);
            continue;
        }
        method_decls.push_back(method.decl);
        method_modules.push_back(method.module);
        method_trait_names.push_back(method.traitName);
    }
    // Concrete owner/impl methods override trait defaults with the same name.
    if (method_decls.empty()) {
        method_decls       = std::move(default_decls);
        method_modules     = std::move(default_modules);
        method_trait_names = std::move(default_trait_names);
    }
    if (method_decls.empty())
        return kInvalidTypeId; // no such method: may still be a callable field

    if (method_decls.size() == 1U && !method_decls.front()->genericParams.empty()) {
        const frontend::Declaration *method_decl = method_decls.front();
        const session::ModuleKey method_module   = method_modules.front();
        PerModuleSema *method_sema =
            owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
        const auto saved_decl_id   = currentDeclId_;
        const auto saved_kind      = currentFunctionKind_;
        const size_t provided_args = call.operands.size() - 1U;
        const bool has_receiver_entry =
            !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
        const bool generic_decl_is_slice =
            !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
        const auto *generic_method_fn =
            method_sema != nullptr ? type_table.function(method_sema->typeOfDecl(method_decl->id))
                                   : nullptr;
        const size_t generic_slice_param_index =
            generic_decl_is_slice && generic_method_fn != nullptr
                ? generic_method_fn->params.size() - 1U
                : (generic_method_fn != nullptr ? generic_method_fn->params.size()
                                                : method_decl->parameters.size());
        const size_t generic_fixed_explicit =
            generic_decl_is_slice ? generic_slice_param_index - (has_receiver_entry ? 1U : 0U)
                                  : generic_slice_param_index - (has_receiver_entry ? 1U : 0U);
        const bool generic_explicit_slice = [&]() {
            if (!generic_decl_is_slice || provided_args != generic_fixed_explicit + 1U ||
                call.operands.empty())
                return false;
            const TypeId last = resolve(inferExpr(call.operands.back()));
            return type_table.slice(last) != nullptr || type_table.array(last) != nullptr;
        }();
        const bool generic_auto_collect = generic_decl_is_slice && !generic_explicit_slice;
        if (provided_args < generic_fixed_explicit ||
            (!generic_decl_is_slice && provided_args != generic_fixed_explicit) ||
            (generic_decl_is_slice && !generic_auto_collect && !generic_explicit_slice &&
             provided_args != generic_fixed_explicit)) {
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
                    ? methodSelfParamType(method_decl->parameters.front())
                    : (is_pointer ? base_type : type_table.internPointer(pointee));
            argument_types.push_back(self_type);
        }
        for (size_t index = has_receiver_entry ? 1U : 0U; index < method_decl->parameters.size();
             ++index) {
            if (generic_decl_is_slice && index == method_decl->parameters.size() - 1U) {
                // Make the variadic slice available to generic inference as a
                // `[]T` shape. When the caller passed an explicit slice/array,
                // use its full type; otherwise infer `T` from the first tail
                // element so `pick<i32>(10, 20, 30)` stays valid.
                if (generic_explicit_slice && !call.operands.empty()) {
                    argument_types.push_back(inferExpr(call.operands.back()));
                } else if (call.operands.size() > generic_fixed_explicit + 1U) {
                    const TypeId element = inferExpr(call.operands[generic_fixed_explicit + 1U]);
                    argument_types.push_back(type_table.internSlice(element));
                } else {
                    argument_types.push_back(type_table.internSlice(kInvalidTypeId));
                }
            } else if (index + 1U < call.operands.size())
                argument_types.push_back(inferExpr(call.operands[index + 1U]));
        }

        const auto *method_fn = method_sema != nullptr
                                    ? type_table.function(method_sema->typeOfDecl(method_decl->id))
                                    : nullptr;
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
                instantiations != nullptr
                    ? instantiations->bindCall(module, callee.id, method_module, method_decl->id,
                                               inferred_args)
                    : ~size_t{0};
            if (instance_index == ~size_t{0}) {
                report(call.span, "too many generic instantiations",
                       diagnostics::err::GenericExplosion);
                return error_type;
            }
            const TypeId instance_type =
                instantiations->substituteFunction(*method_fn, inferred_args);
            setExprType(callee.id, instance_type);
            setResolvedCallTarget(callee.id, method_module, method_decl->id);
            std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
            const auto *instance_fn = type_table.function(instance_type);
            if (instance_fn != nullptr) {
                const size_t instance_slice_param_index = generic_decl_is_slice
                                                              ? instance_fn->params.size() - 1U
                                                              : instance_fn->params.size();
                for (size_t explicit_index = 0U; explicit_index < generic_fixed_explicit;
                     ++explicit_index) {
                    const size_t param_index =
                        has_receiver_entry ? explicit_index + 1U : explicit_index;
                    const size_t arg_index = explicit_index + 1U;
                    if (arg_index < call.operands.size())
                        (void)checkOwnershipCoercion(call.operands[arg_index],
                                                     instance_fn->params[param_index], seen_roots,
                                                     call.span, true);
                }
                if (generic_auto_collect && generic_decl_is_slice)
                    (void)checkVariadicTailArgs(call.span, call.operands,
                                                instance_fn->params[instance_slice_param_index],
                                                generic_fixed_explicit + 1U, true);
            }
            return instance_fn != nullptr ? instance_fn->result : error_type;
        }
        return error_type;
    }

    const frontend::Declaration *method_decl = method_decls.front();
    session::ModuleKey method_module         = method_modules.front();
    if (method_decls.size() > 1U) {
        // The lowered method type carries the receiver as its first parameter, so
        // selection runs with one implicit argument.
        std::vector<OverloadCandidate> candidates;
        candidates.reserve(method_decls.size());
        for (size_t index = 0; index < method_decls.size(); ++index) {
            const auto *decl    = method_decls[index];
            const auto decl_mod = method_modules[index];
            OverloadCandidate candidate;
            candidate.variadicSlice =
                !decl->parameters.empty() && decl->parameters.back().isVariadicSlice;
            const auto *decl_sema = owner != nullptr ? owner->findModuleSema(decl_mod) : nullptr;
            candidate.type =
                decl_sema != nullptr ? decl_sema->typeOfDecl(decl->id) : typeOfDecl(decl->id);
            candidate.fn     = type_table.function(candidate.type);
            candidate.span   = decl->span;
            candidate.module = decl_mod;
            candidate.decl   = decl->id;
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
            const auto found = std::find(method_decls.begin(), method_decls.end(), method_decl);
            if (found != method_decls.end()) {
                const size_t chosen_index = static_cast<size_t>(found - method_decls.begin());
                method_module             = method_modules[chosen_index];
            }
        }
    }
    setResolvedCallTarget(callee.id, method_module, method_decl->id);
    PerModuleSema *method_sema = owner != nullptr ? owner->findModuleSema(method_module) : nullptr;

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
        self_type = methodSelfParamType(fn_params.front());
    }

    // Check explicit arguments against remaining params.
    // A method with no receiver is static: it expects exactly the explicit
    // call arguments, so `Point.foo()` resolves with zero args.
    const size_t provided_args            = call.operands.size() - 1;
    const bool method_is_vslice           = !fn_params.empty() && fn_params.back().isVariadicSlice;
    const TypeId method_fn_type_for_slice = method_sema != nullptr
                                                ? method_sema->typeOfDecl(method_decl->id)
                                                : typeOfDecl(method_decl->id);
    const auto *method_fn_for_slice       = type_table.function(method_fn_type_for_slice);
    const size_t method_slice_param_index =
        method_is_vslice && method_fn_for_slice != nullptr
            ? method_fn_for_slice->params.size() - 1U
            : (method_fn_for_slice != nullptr ? method_fn_for_slice->params.size()
                                              : fn_params.size());
    const size_t method_fixed_explicit = method_is_vslice
                                             ? method_slice_param_index - (has_receiver ? 1U : 0U)
                                             : method_slice_param_index - (has_receiver ? 1U : 0U);
    const bool method_explicit_slice   = [&]() {
        if (!method_is_vslice || provided_args != method_fixed_explicit + 1U ||
            call.operands.empty())
            return false;
        const TypeId last = resolve(inferExpr(call.operands.back()));
        return type_table.slice(last) != nullptr || type_table.array(last) != nullptr;
    }();
    const bool method_auto_collect = method_is_vslice && !method_explicit_slice;
    if (provided_args < method_fixed_explicit ||
        (!method_is_vslice && provided_args != method_fixed_explicit) ||
        (method_is_vslice && !method_auto_collect && !method_explicit_slice &&
         provided_args != method_fixed_explicit)) {
        report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
        return error_type;
    }
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    const auto method_fn   = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                    : typeOfDecl(method_decl->id);
    const auto *method_ffn = type_table.function(method_fn);
    for (size_t i = 0; i < method_fixed_explicit; ++i) {
        TypeId arg_type          = inferExpr(call.operands[i + 1]);
        TypeId param_type        = error_type;
        const size_t param_index = has_receiver ? i + 1U : i;
        if (method_ffn != nullptr && param_index < method_ffn->params.size()) {
            param_type = method_ffn->params[param_index];
        } else if (has_receiver && i + 1U < fn_params.size()) {
            param_type = borrowParamType(fn_params[i + 1]);
        } else if (!has_receiver && i < fn_params.size()) {
            param_type = borrowParamType(fn_params[i]);
        }
        if (param_type) {
            (void)checkOwnershipCoercion(call.operands[i + 1], param_type, seen_roots, call.span,
                                         true);
        }
        if (param_type && arg_type && !coerceValue(call.operands[i + 1], param_type, arg_type))
            reportCoercionFailure(call.span, param_type, arg_type,
                                  "method call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    if (method_auto_collect && method_is_vslice) {
        const TypeId slice_type = method_ffn != nullptr
                                      ? method_ffn->params[method_slice_param_index]
                                      : typeOfExpr(call.operands.back());
        (void)checkVariadicTailArgs(call.span, call.operands, slice_type,
                                    method_fixed_explicit + 1U, true);
    }

    const TypeId method_type    = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                         : typeOfDecl(method_decl->id);
    const auto *method_fn_final = type_table.function(method_type);
    TypeId result =
        method_fn_final != nullptr
            ? method_fn_final->result
            : (method_sema != nullptr ? kInvalidTypeId : lowerTypeExpr(method_decl->declaredType));
    if (!result)
        result = void_type;
    currentDeclId_       = saved_decl_id;
    currentFunctionKind_ = saved_kind;
    // Record a type for the Field/Arrow callee: it is a resolved method, not a
    // struct field, and the later standalone-expression sweep would otherwise
    // re-infer it as a field access and report "unknown field".
    if (method_type)
        setExprType(callee.id, method_type);
    else
        setExprType(callee.id, result);
    const bool implicit_self = has_receiver && !fn_params.front().type;
    const bool var_self =
        has_receiver && fn_params.front().bindingKind == frontend::BindingKind::Var;
    if (implicit_self || var_self)
        invalidateReceiverRoot(callee.operands[0]);
    return result;
}

void PerModuleSema::invalidateReceiverRoot(frontend::ExprId base) {
    if (!base || base.value > snapshot.expressions().size())
        return;
    const auto *resolved = findResolvedExpr(base);
    if (resolved == nullptr || !resolved->local)
        return;
    movedLocals_.insert(resolved->local.value);
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

bool PerModuleSema::bindingIsVariadicSlice(const session::ResolvedName &binding) noexcept {
    return binding.isVariadicSlice;
}

size_t PerModuleSema::variadicSliceParam(const session::ResolvedName *binding,
                                         const FunctionType *fn) const {
    if (binding == nullptr || fn == nullptr || fn->params.empty() || !binding->isVariadicSlice)
        return fn != nullptr ? fn->params.size() : 0U;
    return fn->params.size() - 1U;
}

TypeId PerModuleSema::checkVariadicTail(frontend::TextSpan span,
                                        const std::vector<frontend::ExprId> &args,
                                        const FunctionType *fn, size_t slice_index,
                                        bool allow_literals) {
    if (fn == nullptr || slice_index >= fn->params.size()) {
        report(span, "variadic slice function has no slice parameter",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    const TypeId slice_type = resolve(fn->params[slice_index]);
    const auto *slice       = type_table.slice(slice_type);
    if (slice == nullptr) {
        report(span, "variadic slice parameter is not a slice type",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    return checkVariadicTailArgs(span, args, fn->params[slice_index], slice_index + 1U,
                                 allow_literals);
}

TypeId PerModuleSema::checkVariadicTailArgs(frontend::TextSpan span,
                                            const std::vector<frontend::ExprId> &args,
                                            TypeId slice_type, size_t first_tail_index,
                                            bool allow_literals) {
    const TypeId resolved_slice = resolve(slice_type);
    const auto *slice           = type_table.slice(resolved_slice);
    if (slice == nullptr) {
        report(span, "variadic slice parameter is not a slice type",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    TypeId element = slice->element;
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t index = first_tail_index; index < args.size(); ++index) {
        const frontend::ExprId arg = args[index];
        TypeId arg_type            = inferExpr(arg);
        (void)checkOwnershipCoercion(arg, slice->element, seen_roots, span, true);
        if (!coerceValue(arg, slice->element, arg_type)) {
            if (!allow_literals || !adaptNumericLiteral(arg, slice->element))
                reportCoercionFailure(span, slice->element, arg_type,
                                      "variadic slice argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
    }
    return type_table.internSlice(element);
}

TypeId PerModuleSema::inferBlock(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId last      = void_type;
    if (inStateBody_) {
        // A `jump` consumes the block terminator. The standalone sweep still
        // visits expressions after it, so stop inferring once we have lowered
        // a terminating state transfer.
        bool terminated_by_state_transfer = false;
        std::vector<frontend::StmtId> pending_defers;
        for (const auto &stmt_id : expr.statements) {
            if (!stmt_id)
                continue;
            if (stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (snapshot.isMacroTemplateStmt(stmt.id))
                continue;
            if (terminated_by_state_transfer)
                break;
            if (stmt.kind == frontend::StmtKind::Expression && stmt.expression) {
                last = inferExpr(stmt.expression);
            } else if (stmt.kind == frontend::StmtKind::Defer) {
                pending_defers.push_back(stmt_id);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Binding) {
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
                const TypeId existing_type = typeOfLocal(stmt.binding.id);
                if (ann_type || stmt.binding.initializer)
                    setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
                else if (!existing_type)
                    setLocalType(stmt.binding.id, invalid_type);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Return) {
                checkReturnStatement(stmt);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Break) {
                checkLoopControl(stmt, true);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Continue) {
                checkLoopControl(stmt, false);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Jump) {
                inferJump(stmt);
                terminated_by_state_transfer = true;
                last                         = void_type;
            } else if (stmt.kind == frontend::StmtKind::Declaration) {
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Expression && stmt.expression &&
                       stmt.expression.value <= snapshot.expressions().size() &&
                       snapshot.expressions()[stmt.expression.value - 1U].kind ==
                           frontend::ExprKind::DockCall) {
                // Handled by the regular expression case above; keep the code
                // structure simple and let the fallthrough infer the call.
            }
        }
        for (const auto stmt_id : pending_defers) {
            if (stmt_id && stmt_id.value <= snapshot.statements().size()) {
                const auto &defer_stmt = snapshot.statements()[stmt_id.value - 1U];
                checkDeferStatement(defer_stmt);
                checkDeferCaptures(defer_stmt, expr);
            }
        }
        return last;
    }
    std::vector<frontend::StmtId> pending_defers;
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
        } else if (stmt.kind == frontend::StmtKind::Defer) {
            pending_defers.push_back(stmt_id);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Binding) {
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
            const TypeId existing_type = typeOfLocal(stmt.binding.id);
            // A for-in element binding is typed by the loop before the body is
            // lowered; keep that type rather than marking the synthetic
            // binding untyped.
            if (ann_type || stmt.binding.initializer)
                setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
            else if (!existing_type)
                setLocalType(stmt.binding.id, invalid_type);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Return) {
            checkReturnStatement(stmt);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Break) {
            checkLoopControl(stmt, true);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Continue) {
            checkLoopControl(stmt, false);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Jump) {
            report(stmt.span, "jump is only allowed inside a state function",
                   diagnostics::err::UnsupportedSyntax);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Declaration) {
            last = void_type;
        }
    }
    for (const auto stmt_id : pending_defers) {
        if (stmt_id && stmt_id.value <= snapshot.statements().size()) {
            const auto &defer_stmt = snapshot.statements()[stmt_id.value - 1U];
            checkDeferStatement(defer_stmt);
            checkDeferCaptures(defer_stmt, expr);
        }
    }
    return last;
}

void PerModuleSema::checkLoopControl(const frontend::Statement &stmt, bool is_break) {
    const char *kind = is_break ? "break" : "continue";
    if (active_loop_labels_.empty()) {
        report(stmt.span, std::string(kind) + " is only allowed inside a loop",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    if (!stmt.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), stmt.label) ==
            active_loop_labels_.end()) {
            report(stmt.span,
                   std::string(kind) + " label '" + stmt.label + "' does not name an active loop",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
}

bool PerModuleSema::deferBodyHasControlFlow(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
                stmt.kind == frontend::StmtKind::Continue ||
                stmt.kind == frontend::StmtKind::Jump) {
                return true;
            }
            if (stmt.expression && deferBodyHasControlFlow(stmt.expression))
                return true;
        }
        return false;
    }
    for (const auto operand : expr.operands) {
        if (deferBodyHasControlFlow(operand))
            return true;
    }
    return false;
}

bool PerModuleSema::deferStatementHasControlFlow(const frontend::Statement &stmt) const noexcept {
    if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
        stmt.kind == frontend::StmtKind::Continue || stmt.kind == frontend::StmtKind::Jump) {
        return true;
    }
    return stmt.expression && deferBodyHasControlFlow(stmt.expression);
}

void PerModuleSema::checkDeferStatement(const frontend::Statement &stmt) {
    if (!stmt.expression) {
        report(stmt.span, "defer requires an expression or block", diagnostics::err::ExpectedExpr);
        return;
    }
    if (deferStatementHasControlFlow(stmt)) {
        report(stmt.span, "deferred body cannot contain return, break, continue, or jump",
               diagnostics::err::UnsupportedSyntax);
    }
    if (stmt.expression.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[stmt.expression.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        // A cleanup block itself produces no value even when its last
        // statement is a value-producing expression.
        (void)inferExpr(stmt.expression);
        setExprType(stmt.expression, void_type);
    } else {
        (void)inferExpr(stmt.expression);
    }
}

void PerModuleSema::checkDeferCaptures(const frontend::Statement &stmt,
                                       const frontend::Expression &block) {
    if (!stmt.expression)
        return;

    struct DirectBinding {
        size_t index  = 0;
        bool has_init = false;
        std::string name;
    };
    std::unordered_map<uint32_t, DirectBinding> direct;
    size_t defer_index = 0;
    bool saw_defer     = false;
    for (size_t index = 0; index < block.statements.size(); ++index) {
        const auto stmt_id = block.statements[index];
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &candidate = snapshot.statements()[stmt_id.value - 1U];
        if (candidate.id == stmt.id && !saw_defer) {
            defer_index = index;
            saw_defer   = true;
        }
        if (candidate.kind == frontend::StmtKind::Binding && candidate.binding.id) {
            direct[candidate.binding.id.value] = DirectBinding{
                index, candidate.binding.initializer ? true : false, candidate.binding.name};
        }
    }
    if (!saw_defer)
        return;

    const auto hasEarlyExit = [&](const size_t before_index) {
        for (size_t index = 0; index < before_index; ++index) {
            const auto stmt_id = block.statements[index];
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &before = snapshot.statements()[stmt_id.value - 1U];
            if (before.kind == frontend::StmtKind::Return ||
                before.kind == frontend::StmtKind::Break ||
                before.kind == frontend::StmtKind::Continue ||
                before.kind == frontend::StmtKind::Jump) {
                return true;
            }
            // Control flow nested inside expressions may or may not run before
            // the binding initializer; only direct exits are guaranteed to be
            // an uninitialized-capture hazard per the current validation rule.
            if (before.expression && deferBodyHasControlFlow(before.expression))
                return true;
        }
        return false;
    };

    const auto walk = [&](const auto &self, const frontend::ExprId expr_id) -> void {
        if (!expr_id || expr_id.value > snapshot.expressions().size())
            return;
        const auto &expr = snapshot.expressions()[expr_id.value - 1U];
        if (expr.kind == frontend::ExprKind::Name) {
            const auto *resolved = findResolvedExpr(expr_id);
            if (resolved != nullptr && resolved->local) {
                const auto found = direct.find(resolved->local.value);
                if (found != direct.end() && found->second.index > defer_index) {
                    if (!found->second.has_init || hasEarlyExit(found->second.index)) {
                        report(expr.span,
                               "defer may run before captured binding '" + found->second.name +
                                   "' is initialized",
                               diagnostics::err::UnsupportedSyntax);
                    }
                }
            }
        }
        for (const auto operand : expr.operands)
            self(self, operand);
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &inner_stmt = snapshot.statements()[stmt_id.value - 1U];
            if (inner_stmt.expression)
                self(self, inner_stmt.expression);
            if (inner_stmt.kind == frontend::StmtKind::Binding && inner_stmt.binding.initializer) {
                self(self, inner_stmt.binding.initializer);
            }
        }
    };
    // Traverse macro expansions and raw splices too; they are real deferred
    // code once expanded and resolved.
    walk(walk, stmt.expression);
    const auto &defer_expr = snapshot.expressions()[stmt.expression.value - 1U];
    if (defer_expr.expansion)
        walk(walk, defer_expr.expansion);
}

TypeId PerModuleSema::inferIf(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    optionalPropInCondition_ = true;
    TypeId cond              = inferExpr(expr.operands[0]);
    optionalPropInCondition_ = false;
    if (!sameType(cond, bool_type))
        report(expr.span, "if condition must be boolean", diagnostics::err::TypeMismatch);
    const auto &condition = snapshot.expressions()[expr.operands[0].value - 1U];
    frontend::LocalId narrowed_local;
    TypeId original_local_type = kInvalidTypeId;
    TypeId narrowed_type       = kInvalidTypeId;
    bool narrow_then           = false;
    if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
        condition.cast_type) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            narrowed_local      = resolved->local;
            original_local_type = typeOfLocal(narrowed_local);
            narrowed_type       = lowerTypeExpr(condition.cast_type);
            if (narrowed_type) {
                narrow_then = true;
            }
        }
    } else if (condition.kind == frontend::ExprKind::IsNull && !condition.operands.empty()) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            const TypeId optional = resolve(typeOfLocal(resolved->local));
            if (const auto *opt = type_table.optional(optional)) {
                narrowed_local      = resolved->local;
                original_local_type = typeOfLocal(narrowed_local);
                narrowed_type       = type_table.stripQualifiers(opt->inner);
                // `x is null` proves the payload type in the `else` branch.
            }
        }
    } else if (condition.kind == frontend::ExprKind::Unary && condition.text == "not" &&
               !condition.operands.empty()) {
        const auto &inner = snapshot.expressions()[condition.operands[0].value - 1U];
        if (inner.kind == frontend::ExprKind::IsNull && !inner.operands.empty()) {
            const auto *resolved = findResolvedExpr(inner.operands[0]);
            if (resolved != nullptr && resolved->local) {
                const TypeId optional = resolve(typeOfLocal(resolved->local));
                if (const auto *opt = type_table.optional(optional)) {
                    narrowed_local      = resolved->local;
                    original_local_type = typeOfLocal(narrowed_local);
                    narrowed_type       = type_table.stripQualifiers(opt->inner);
                    narrow_then         = true;
                }
            }
        }
    }

    if (narrowed_local && narrowed_type && narrow_then)
        setLocalType(narrowed_local, narrowed_type);
    TypeId then_type = inferExpr(expr.operands[1]);
    if (narrowed_local && narrowed_type)
        setLocalType(narrowed_local, original_local_type);
    if (narrowed_local && narrowed_type && !narrow_then)
        setLocalType(narrowed_local, narrowed_type);
    TypeId else_type = expr.operands.size() >= 3 ? inferExpr(expr.operands[2]) : void_type;
    if (narrowed_local && narrowed_type) {
        setLocalType(narrowed_local, original_local_type);
    }
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
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);
    if (!expr.operands.empty()) {
        optionalPropInCondition_ = true;
        TypeId cond              = inferExpr(expr.operands[0]);
        optionalPropInCondition_ = false;
        if (!sameType(cond, bool_type))
            report(expr.span, "loop condition must be boolean", diagnostics::err::TypeMismatch);
    }
    // The body must be inferred too, otherwise locals declared inside the loop never get a type.
    if (expr.operands.size() >= 2U)
        (void)inferExpr(expr.operands[1]);
    active_loop_labels_.pop_back();
    return void_type;
}

TypeId PerModuleSema::inferFor(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);
    // operands: [cond, body, step].
    if (!expr.operands.empty()) {
        optionalPropInCondition_ = true;
        TypeId cond              = inferExpr(expr.operands[0]);
        optionalPropInCondition_ = false;
        if (!sameType(cond, bool_type))
            report(expr.span, "loop condition must be boolean", diagnostics::err::TypeMismatch);
    }
    if (expr.operands.size() >= 2U && expr.operands[1])
        (void)inferExpr(expr.operands[1]);
    if (expr.operands.size() >= 3U && expr.operands[2])
        (void)inferExpr(expr.operands[2]);
    active_loop_labels_.pop_back();
    return void_type;
}

TypeId PerModuleSema::inferForIn(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2U)
        return void_type;
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);

    const TypeId iterable_type = inferExpr(expr.operands[0]);
    if (!iterable_type || type_table.kindOf(resolve(iterable_type)) == TypeKind::Error) {
        active_loop_labels_.pop_back();
        return void_type;
    }

    TypeId pointee = resolve(type_table.stripQualifiers(iterable_type));
    if (type_table.kindOf(pointee) == TypeKind::Pointer) {
        if (const auto *ptr = type_table.pointer(pointee)) {
            pointee = resolve(type_table.stripQualifiers(ptr->pointee));
        }
    } else if (type_table.kindOf(pointee) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(pointee)) {
            pointee = resolve(type_table.stripQualifiers(opt->inner));
            if (type_table.kindOf(pointee) == TypeKind::Pointer) {
                if (const auto *ptr = type_table.pointer(pointee))
                    pointee = resolve(type_table.stripQualifiers(ptr->pointee));
            }
        }
    }

    const StructType *st = type_table.struct_type(pointee);
    if (st == nullptr) {
        active_loop_labels_.pop_back();
        report(expr.span, "iterated value is not a struct with iterator methods",
               diagnostics::err::TypeMismatch);
        return void_type;
    }
    std::string owner_name(st->name);
    if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
        owner_name.resize(angle);

    const auto resolved_methods            = findMethodsForOwner(owner_name, "next");
    const frontend::Declaration *next_decl = nullptr;
    session::ModuleKey next_module;
    for (const auto &method : resolved_methods) {
        if (method.decl == nullptr || method.decl->parameters.empty())
            continue;
        if (method.decl->parameters.front().name != "self")
            continue;
        next_decl   = method.decl;
        next_module = method.module;
        break;
    }

    if (next_decl == nullptr)
        report(expr.span,
               "iterator type '" + type_table.typeToString(iterable_type) +
                   "' is missing a 'next' method",
               diagnostics::err::TypeMismatch);

    TypeId element_type    = error_type;
    TypeId union_type      = error_type;
    uint32_t element_index = 0;
    uint32_t end_index     = static_cast<uint32_t>(-1);
    bool found_end         = false;
    bool valid_union       = false;
    if (next_decl != nullptr) {
        const PerModuleSema *next_sema =
            owner != nullptr ? owner->findModuleSema(next_module) : nullptr;
        const TypeId next_fn =
            next_sema != nullptr ? next_sema->typeOfDecl(next_decl->id) : typeOfDecl(next_decl->id);
        const auto *fn = type_table.function(next_fn);
        if (fn == nullptr) {
            report(expr.span, "iterator 'next' method has no function type",
                   diagnostics::err::TypeMismatch);
        } else {
            const TypeId result = resolve(fn->result);
            const auto *uf      = type_table.union_type(result);
            if (uf == nullptr || !uf->is_tagged) {
                report(expr.span,
                       "iterator 'next' method must return a tagged union with one value member "
                       "and 'End'",
                       diagnostics::err::TypeMismatch);
            } else if (uf->members.size() != 2U) {
                report(
                    expr.span,
                    "iterator 'next' return union must have exactly two members: a value and 'End'",
                    diagnostics::err::TypeMismatch);
            } else {
                union_type = fn->result;
                for (uint32_t index = 0; index < uf->members.size(); ++index) {
                    const TypeId member_type = resolve(uf->members[index]);
                    if (sameType(member_type, end_type)) {
                        end_index = index;
                        found_end = true;
                    } else {
                        element_index = index;
                        element_type  = uf->members[index];
                    }
                }
                const bool has_end   = found_end;
                const bool has_value = element_type != error_type;
                if (has_end && has_value) {
                    valid_union = true;
                } else {
                    report(expr.span,
                           "iterator 'next' return union must contain a value member and 'End'",
                           diagnostics::err::TypeMismatch);
                }
            }
        }
    }

    if (next_decl == nullptr || !valid_union) {
        active_loop_labels_.pop_back();
        return void_type;
    }

    typed_map.forInNext.insert(id.value, TypedMap::ForInNext{next_module, next_decl->id});
    typed_map.forInElementIndex.insert(id.value, element_index);
    typed_map.forInEndIndex.insert(id.value, end_index);
    typed_map.forInUnionType.insert(id.value, union_type);

    if (expr.forInBinding) {
        const TypeId ann    = lowerTypeExpr(expr.cast_type);
        const TypeId actual = element_type;
        if (ann && !sameType(ann, actual)) {
            report(expr.span, "iterator element type does not match loop variable annotation",
                   diagnostics::err::TypeMismatch);
            setLocalType(expr.forInBinding, actual);
        } else {
            setLocalType(expr.forInBinding, ann ? ann : actual);
        }
    }
    (void)inferExpr(expr.operands[1]);
    active_loop_labels_.pop_back();
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
        // Bare `opaque` is tagged: any concrete value erases into it, and
        // checked extraction returns `?T` so the tag-match/none path is visible.
        if (type_table.kindOf(to_resolved) == TypeKind::Opaque) {
            if (type_table.kindOf(from_resolved) == TypeKind::Opaque)
                return result;
            return type_table.internOpaque();
        }
        if (type_table.kindOf(from_resolved) == TypeKind::Opaque) {
            if (type_table.kindOf(to_resolved) == TypeKind::Opaque)
                return result;
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
                if (narrowed_type)
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
    if (coercesTo(target, source)) {
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
        return true;
    }
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

uint32_t PerModuleSema::stateMachineIdFor(const frontend::Declaration &decl) {
    const auto existing = stateMachineByDecl_.find(decl.id.value);
    if (existing != stateMachineByDecl_.end())
        return existing->second;
    if (decl.parentScope) {
        const auto local = localStateMachineByParent_.find(decl.parentScope.value);
        if (local != localStateMachineByParent_.end() && local->second != 0U) {
            stateMachineByDecl_[decl.id.value] = local->second;
            return local->second;
        }
        const uint32_t machine_id = nextStateMachineId_++;
        localStateMachineByParent_.emplace(decl.parentScope.value, machine_id);
        stateMachineByDecl_[decl.id.value] = machine_id;
        return machine_id;
    }
    const TypeId fn_type = typeOfDecl(decl.id);
    const auto *fn       = type_table.function(fn_type);
    if (fn == nullptr)
        return 0;
    const uint32_t key = fn->result.intern_seq;
    auto &machine_id   = stateMachineByReturn_[key];
    if (machine_id == 0)
        machine_id = nextStateMachineId_++;
    stateMachineByDecl_[decl.id.value] = machine_id;
    return machine_id;
}

uint32_t PerModuleSema::stateMachineIdOf(const frontend::Declaration &decl) const noexcept {
    const auto existing = stateMachineByDecl_.find(decl.id.value);
    return existing != stateMachineByDecl_.end() ? existing->second : 0U;
}

namespace {

const frontend::Declaration *findDeclarationForResolved(const PerModuleSema &sema,
                                                        const session::ResolvedName &resolved) {
    const session::ModuleKey target_module =
        resolved.target.module.empty() ? sema.module : resolved.target.module;
    PerModuleSema *target =
        sema.owner != nullptr ? sema.owner->findModuleSema(target_module) : nullptr;
    if (target == nullptr)
        return nullptr;
    frontend::DeclId decl_id = resolved.declaration;
    if (!decl_id && resolved.target.localSymbol)
        decl_id = frontend::DeclId{resolved.target.localSymbol.value};
    if (!decl_id || decl_id.value > target->snapshot.declarations().size())
        return nullptr;
    return &target->snapshot.declarations()[decl_id.value - 1U];
}

} // namespace

void PerModuleSema::inferDockCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId result    = error_type;
    if (!expr.operands.empty()) {
        const auto *resolved                = findResolvedExpr(expr.operands[0]);
        const frontend::Declaration *target = nullptr;
        if (resolved != nullptr) {
            target = findDeclarationForResolved(*this, *resolved);
        }
        if (target == nullptr || target->kind != frontend::DeclKind::Function ||
            target->functionKind != frontend::FunctionKind::State) {
            report(expr.span, "dock target must be a state function",
                   diagnostics::err::UnsupportedSyntax);
        } else {
            const TypeId target_type = typeOfResolvedName(expr.operands[0]);
            if (const auto *fn = type_table.function(target_type)) {
                result = fn->result;
                const bool target_is_slice =
                    resolved != nullptr && resolved->isVariadicSlice && !fn->params.empty();
                const size_t slice_index =
                    target_is_slice ? fn->params.size() - 1U : fn->params.size();
                if (!target_is_slice && expr.operands.size() - 1U != fn->params.size()) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                }
                const bool explicit_slice_arg =
                    target_is_slice && expr.operands.size() - 1U == slice_index + 1U &&
                    type_table.slice(resolve(inferExpr(expr.operands[slice_index + 1U]))) !=
                        nullptr;
                const bool auto_collected =
                    target_is_slice &&
                    (expr.operands.size() - 1U > slice_index + 1U ||
                     (expr.operands.size() - 1U == slice_index + 1U && !explicit_slice_arg));
                if (target_is_slice && expr.operands.size() - 1U < slice_index) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                    result = fn->result;
                } else if (target_is_slice && !auto_collected &&
                           expr.operands.size() - 1U != fn->params.size()) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                } else {
                    const size_t checked_params = target_is_slice ? slice_index : fn->params.size();
                    for (size_t index = 0;
                         index < checked_params && index + 1U < expr.operands.size(); ++index) {
                        const TypeId arg_type = inferExpr(expr.operands[index + 1U]);
                        if (!coerceValue(expr.operands[index + 1U], fn->params[index], arg_type)) {
                            reportCoercionFailure(expr.span, fn->params[index], arg_type,
                                                  "dock argument type mismatch",
                                                  diagnostics::err::NoMatchingFn);
                        }
                    }
                    if (auto_collected)
                        (void)checkVariadicTail(expr.span, expr.operands, fn, slice_index, true);
                }
                setExprType(expr.operands[0], target_type);
                setResolvedCallTarget(expr.operands[0],
                                      resolved != nullptr ? resolved->target.module
                                                          : session::ModuleKey{},
                                      target->id);
                if (resolved != nullptr && resolved->target.module.empty())
                    setResolvedCallTarget(expr.operands[0], module, target->id);
            }
        }
    }
    setExprType(id, result);
}

void PerModuleSema::inferJump(const frontend::Statement &stmt) {
    if (!inStateBody_) {
        report(stmt.span, "jump is only allowed inside a state function",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const auto *resolved =
        stmt.label.empty()
            ? nullptr
            : session::lookupBinding(resolution, stmt.label, currentBodyScope_, snapshot.scopes());
    const frontend::Declaration *target = nullptr;
    if (resolved != nullptr)
        target = findDeclarationForResolved(*this, *resolved);
    if (target == nullptr) {
        report(stmt.span, "jump target must be a state function: '" + stmt.label + "'",
               diagnostics::err::UndefinedIdent);
        return;
    }
    if (target->functionKind != frontend::FunctionKind::State) {
        report(stmt.span, "jump target must be a state function",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const uint32_t target_machine = stateMachineIdFor(*target);
    if (target_machine != currentStateMachineId_) {
        report(stmt.span, "jump target must be in the same state machine",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const TypeId target_type = typeOfResolvedBinding(*resolved);
    const auto *fn           = type_table.function(target_type);
    if (fn == nullptr)
        return;
    const bool target_is_slice =
        resolved != nullptr && resolved->isVariadicSlice && !fn->params.empty();
    const size_t slice_index = target_is_slice ? fn->params.size() - 1U : fn->params.size();
    if (!target_is_slice && stmt.arguments.size() != fn->params.size()) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const bool explicit_slice_arg =
        target_is_slice && stmt.arguments.size() == slice_index + 1U &&
        type_table.slice(resolve(inferExpr(stmt.arguments[slice_index]))) != nullptr;
    const bool auto_collected =
        target_is_slice && (stmt.arguments.size() > slice_index + 1U ||
                            (stmt.arguments.size() == slice_index + 1U && !explicit_slice_arg));
    if (target_is_slice && stmt.arguments.size() < slice_index) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    } else if (target_is_slice && !auto_collected && stmt.arguments.size() != fn->params.size()) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const size_t checked_params = target_is_slice ? slice_index : fn->params.size();
    for (size_t index = 0; index < checked_params && index < stmt.arguments.size(); ++index) {
        const TypeId arg_type = inferExpr(stmt.arguments[index]);
        if (!coerceValue(stmt.arguments[index], fn->params[index], arg_type)) {
            reportCoercionFailure(stmt.span, fn->params[index], arg_type,
                                  "state transition argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
        }
    }
    if (auto_collected)
        (void)checkVariadicTail(stmt.span, stmt.arguments, fn, slice_index, true);
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
    frontend::ExprId root = assignmentRoot(target);
    if (!root)
        return;

    // `self: view Owner` / `p: view Owner` lower to `*view Owner`, so both
    // `self->x = ...` and dot-style `p.x = ...` report a read-only write.
    [&]() {
        for (frontend::ExprId current = target;
             current && current.value <= snapshot.expressions().size() && current != root;) {
            const auto &cursor = snapshot.expressions()[current.value - 1U];
            if (cursor.kind == frontend::ExprKind::Arrow) {
                root = cursor.operands[0];
                return true;
            }
            if (cursor.kind != frontend::ExprKind::Field &&
                cursor.kind != frontend::ExprKind::Index)
                break;
            if (cursor.operands.empty())
                break;
            current = cursor.operands[0];
        }
        return false;
    }();

    const TypeId declared = typeOfExpr(root) ? typeOfExpr(root) : typeOfResolvedName(root);
    if (!declared)
        return;

    const auto *qual = type_table.qualified(type_table.canonical(declared));
    bool is_view     = qual != nullptr && qual->ownership == types::OwnershipKind::View;
    if (!is_view && type_table.kindOf(resolve(declared)) == TypeKind::Pointer) {
        // The root may be `*view Owner`; strip the pointer layer before
        // checking the pointee qualifier.
        const TypeId resolved_root = resolve(declared);
        const auto *pointer        = type_table.pointer(resolved_root);
        if (pointer != nullptr) {
            const auto *pointee_qual = type_table.qualified(type_table.canonical(pointer->pointee));
            is_view =
                pointee_qual != nullptr && pointee_qual->ownership == types::OwnershipKind::View;
        }
    }
    if (!is_view)
        return;
    const auto &root_expr = snapshot.expressions()[root.value - 1U];
    report(span, "cannot write through '" + root_expr.text + "': a 'view' binding is read-only",
           diagnostics::err::WriteThroughView);
}

void PerModuleSema::checkImmutableRootFieldWrite(frontend::ExprId target, frontend::TextSpan span) {
    const frontend::ExprId root = assignmentRoot(target);
    const auto *root_resolved   = root ? findResolvedExpr(root) : nullptr;
    if (root_resolved == nullptr || (root_resolved->bindingKind != frontend::BindingKind::Let &&
                                     root_resolved->bindingKind != frontend::BindingKind::Const))
        return;
    // Parameters are immutable by default. `var p` is locally mutable; `lend`
    // and explicit pointer/`view` receivers keep their own ownership checks.
    if (root_resolved->local && root_resolved->declKind == frontend::DeclKind::Function &&
        root_resolved->bindingKind == frontend::BindingKind::Let) {
        TypeId param_type     = typeOfLocal(root_resolved->local);
        const TypeId stripped = type_table.stripQualifiers(param_type);
        if (type_table.kindOf(resolve(stripped)) == TypeKind::Pointer) {
            // Explicit `self: *Owner` and `self: lend/view Owner` stay mutable
            // through the pointer path. Bare `self`/`var self` map to *Owner
            // too, but bare self is read-only and var self is mutable; the
            // binding kind distinguishes them.
            if (isBorrowParamType(param_type))
                return;
            bool explicit_pointer = false;
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Function)
                    continue;
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    if (decl.parameters[index].id != root_resolved->local)
                        continue;
                    // Bare `self` is read-only even though it lowers to *Owner.
                    explicit_pointer =
                        !(decl.parameters[index].name == "self" && !decl.parameters[index].type);
                    break;
                }
                if (explicit_pointer)
                    break;
            }
            if (explicit_pointer)
                return;
        } else if (const auto *param_qual =
                       type_table.qualified(type_table.canonical(param_type))) {
            // `lend`/`view` parameters opt out of the default immutable local
            // binding; view is still reported by `checkAssignableOwnership`.
            if (param_qual->ownership == types::OwnershipKind::Lend ||
                param_qual->ownership == types::OwnershipKind::View)
                return;
        }
    }
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
    checkMovedRoot(expr);
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
        if (left_resolved->foreignConstant != nullptr) {
            report(expr.span, "Zith--: cannot assign to an imported C constant",
                   diagnostics::err::UnsupportedSyntax);
        } else if (left_resolved->declaration &&
                   left_resolved->declKind == frontend::DeclKind::Variable &&
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

void PerModuleSema::checkMovedRoot(const frontend::Expression &target) {
    if (target.kind != frontend::ExprKind::Assign || target.operands.size() < 2U)
        return;
    const frontend::ExprId root = assignmentRoot(target.operands[0]);
    if (!root)
        return;
    const auto *resolved = findResolvedExpr(root);
    if (resolved == nullptr || !resolved->local || !movedLocals_.contains(resolved->local.value))
        return;

    bool direct_rebind = target.operands[0] == root;
    if (direct_rebind) {
        movedLocals_.erase(resolved->local.value);
        return;
    }
    const auto &root_expr = snapshot.expressions()[root.value - 1U];
    report(target.span,
           "cannot assign through '" + root_expr.text + "' after it was moved by a previous call",
           diagnostics::err::UseAfterMove);
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
    if (optionalPropInCondition_)
        return bool_type;
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
        if (type_table.kindOf(resolved_target) == TypeKind::Dyn) {
            const auto *dyn = type_table.dyn_type(resolved_target);
            if (dyn != nullptr)
                result = satisfiesConformance(resolve(source), resolve(dyn->target)) &&
                         type_table.struct_type(resolve(source)) != nullptr;
        } else if (type_table.kindOf(resolved_target) == TypeKind::Optional) {
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
    if (ka == TypeKind::Opaque)
        return true;
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
    if (resolved->foreignConstant != nullptr)
        return lowerForeignConstantType(*resolved->foreignConstant);
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

        auto &typed_map = typedMap(artifact.key);

        auto *sema =
            arena_.make<PerModuleSema>(artifact.key, *artifact.frontend, *resolution, type_table_,
                                       typed_map, arena_, artifact.fileId, this);
        sema->instantiations = instantiation_pass_;
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
