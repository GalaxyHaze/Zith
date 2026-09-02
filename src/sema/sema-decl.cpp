#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

namespace zith::sema::modern {

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
    f32_type    = registerPrimitive("f32", TypeKind::Float, 32, true);
    f64_type    = registerPrimitive("f64", TypeKind::Float, 64, true);
    opaque_type = type_table.internOpaque();
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
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Struct),
                                         module);
            break;
        case frontend::DeclKind::Enum:
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Enum),
                                         module);
            break;
        case frontend::DeclKind::Union:
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Union),
                                         module);
            break;
        case frontend::DeclKind::Trait:
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Trait),
                                         module);
            break;
        case frontend::DeclKind::Interface:
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Trait),
                                         module);
            break;
        case frontend::DeclKind::TypeAlias:
            type_table.setDefiningModule(type_table.findOrCreateNamed(decl.name, TypeKind::Alias),
                                         module);
            break;
        default:
            break;
        }
    }
}
void PerModuleSema::prepareImplementOwners() {
    for (const auto &record : snapshot.implementRecords()) {
        const frontend::TypeExprId id = record.ownerType;
        if (!id || id.value > snapshot.typeExpressions().size())
            continue;
        const auto &owner_expr = snapshot.typeExpressions()[id.value - 1U];
        // Named owners are registered by `registerNamedTypes`; composites are
        // interned here so `self` on primitive/optional/slice methods resolves.
        TypeId owner_type = kInvalidTypeId;
        if (owner_expr.kind == frontend::TypeExprKind::Name) {
            owner_type = type_table.lookupNamed(record.owner);
        } else {
            owner_type = lowerTypeExpr(id);
            type_table.registerNamed(record.owner, owner_type);
        }
        if (owner_type)
            implementOwnerTypes_[record.owner] = owner_type;
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
                owner_type = ownerTypeFromName(decl.ownerName);
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
                        // `self` (without a type) is shorthand for `*Owner`,
                        // except a pointer owner such as `*char` is already a
                        // pointer and therefore receives itself by value.
                        ptype = type_table.kindOf(type_table.stripQualifiers(owner_type)) ==
                                        TypeKind::Pointer
                                    ? owner_type
                                    : type_table.internPointer(owner_type);
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
void PerModuleSema::checkImplementBlocks() {
    // Group method declarations by `(owner, trait)` so an empty implement block
    // is still checked for duplicate implementation and missing requirements.
    struct ImplGroup {
        TypeId ownerType = kInvalidTypeId;
        std::string ownerName;
        const frontend::Declaration *trait = nullptr;
        std::vector<const frontend::Declaration *> methods;
        frontend::TextSpan span;
        std::vector<frontend::TextSpan> spans;
    };
    memory::FlatMap<uint64_t, ImplGroup> groups;

    for (const auto &record : snapshot.implementRecords()) {
        const uint64_t key =
            (static_cast<uint64_t>(std::hash<std::string_view>{}(record.owner)) << 32U) ^
            static_cast<uint32_t>(std::hash<std::string_view>{}(record.traitName));
        const frontend::Declaration *trait =
            findDeclNamed(record.traitName, frontend::DeclKind::Trait);
        if (trait == nullptr)
            trait = findDeclNamed(record.traitName, frontend::DeclKind::Interface);
        auto &group = groups[key];
        if (group.trait == nullptr) {
            group.span      = record.span;
            group.ownerName = record.owner;
            group.ownerType = ownerTypeFromName(record.owner);
            group.trait     = trait;
        }
        group.spans.push_back(record.span);
    }
    // Attach methods to the implementation group. A declaration's `traitName`
    // is non-empty only for methods lowered from an implement block.
    memory::FlatSet<std::string> local_implement_owners;
    for (const auto &record : snapshot.implementRecords())
        local_implement_owners.insert(record.owner);
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || decl.traitName.empty())
            continue;
        // Implement methods are attached to the module that declared them;
        // imported methods are already validated by their defining module and
        // have no local implement record to attach to.
        if (!decl.ownerName.empty() &&
            findDeclNamed(decl.ownerName, frontend::DeclKind::Trait) == nullptr &&
            !local_implement_owners.contains(decl.ownerName))
            continue;
        const uint64_t key =
            (static_cast<uint64_t>(std::hash<std::string_view>{}(decl.ownerName)) << 32U) ^
            static_cast<uint32_t>(std::hash<std::string_view>{}(decl.traitName));
        auto *found = groups.get(key);
        if (found)
            found->methods.push_back(&decl);
    }

    for (auto entry : groups) {
        auto &group = entry.second;
        // Implement blocks belong to the module that declares them. Imported
        // modules also re-list implement records through their own types; the
        // defining module already validated the signatures, so revalidating
        // them from a consumer would needlessly import-require the same trait
        // and can reject a valid primitive/slice owner.
        if (group.ownerName.empty())
            continue;
        bool local_trait_decl = false;
        bool local_owner_decl = false;
        for (const auto &decl : snapshot.declarations()) {
            if (decl.kind == frontend::DeclKind::Trait && decl.name == group.trait->name)
                local_trait_decl = true;
            if (decl.kind == frontend::DeclKind::Struct || decl.kind == frontend::DeclKind::Enum ||
                decl.kind == frontend::DeclKind::Union) {
                if (decl.name == group.ownerName)
                    local_owner_decl = true;
            }
        }
        if (!local_trait_decl && !local_owner_decl)
            continue;
        if (group.trait == nullptr) {
            report(group.span, "type after 'as'/'for' is not a declared trait or interface",
                   diagnostics::err::NotATrait);
            continue;
        }
        if (!group.ownerType) {
            report(group.span, "owner type '" + group.ownerName + "' is not defined",
                   diagnostics::err::UndefinedIdent);
            continue;
        }
        if (group.spans.size() > 1U) {
            report(group.spans[1],
                   "duplicate implementation of trait '" + group.trait->name + "' for type '" +
                       group.ownerName + "'",
                   diagnostics::err::DuplicateImplementation);
            continue;
        }
        if (group.trait->kind == frontend::DeclKind::Interface) {
            report(group.span, "interfaces are structural and cannot be implemented explicitly",
                   diagnostics::err::InterfaceMethodNotAllowed);
            continue;
        }

        const TypeId owner_type = type_table.canonical(group.ownerType);
        const TypeId trait_type = type_table.lookupNamed(group.trait->name);
        if (!owner_type || !trait_type)
            continue;
        type_table.conformanceTable().registerConformance(owner_type, trait_type);
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

            // `impl` comes from this module's implement statement, so its
            // lowered signature lives in this module's typed map. Searching
            // other modules by declaration id is unsafe: ids restart per
            // module and can resolve to an unrelated method with the same id.
            PerModuleSema *impl_sema = this;
            const TypeId impl_fn =
                impl_sema != nullptr ? impl_sema->typeOfDecl(impl->id) : typeOfDecl(impl->id);
            const TypeId req_fn   = typeOfDecl(requirement.id);
            const auto *impl_type = type_table.function(impl_fn);
            const auto *req_type  = type_table.function(req_fn);
            if (impl_type == nullptr || req_type == nullptr)
                continue;

            bool matched = sameType(substituteSelf(impl_type->result, owner_type, trait_type),
                                    substituteSelf(req_type->result, owner_type, trait_type));
            if (matched) {
                // Trait requirements declare an implicit `*Self` receiver after
                // lowering. Implement methods may spell that receiver either as
                // an implicit `self` (which the type table turns into
                // `*Owner`) or as the owner value/slice itself; for a primitive
                // or slice owner both spellings are intentionally acceptable.
                size_t impl_index = 0;
                size_t req_index  = 0;
                if (impl_index < impl_type->params.size() && req_index < req_type->params.size()) {
                    const TypeId expected_first =
                        substituteSelf(req_type->params[0], owner_type, trait_type);
                    const auto *first_ptr = type_table.pointer(resolve(expected_first));
                    const bool requirement_is_owner_ptr =
                        first_ptr != nullptr && sameType(resolve(first_ptr->pointee), owner_type);
                    const TypeId impl_first =
                        substituteSelf(impl_type->params[0], owner_type, trait_type);
                    const auto *impl_first_ptr = type_table.pointer(resolve(impl_first));
                    const bool impl_first_is_owner =
                        sameType(impl_first, owner_type) ||
                        (impl_first_ptr != nullptr &&
                         sameType(resolve(impl_first_ptr->pointee), owner_type));
                    if (requirement_is_owner_ptr && impl_first_is_owner) {
                        ++impl_index;
                        ++req_index;
                    } else if (requirement_is_owner_ptr) {
                        ++req_index;
                    }
                }
                for (; impl_index < impl_type->params.size() && req_index < req_type->params.size();
                     ++impl_index, ++req_index) {
                    if (!sameType(
                            substituteSelf(impl_type->params[impl_index], owner_type, trait_type),
                            substituteSelf(req_type->params[req_index], owner_type, trait_type))) {
                        matched = false;
                        break;
                    }
                }
                if (impl_index != impl_type->params.size() ||
                    (req_index != req_type->params.size() && req_type->params.size() != 0U))
                    matched = false;
            }
            if (!matched) {
                report(impl->span,
                       "method '" + impl->name + "' does not match trait '" + group.trait->name +
                           "' requirement signature",
                       diagnostics::err::TraitMethodSignatureMismatch);
            }
        }

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
void PerModuleSema::checkFunctionDefaults() {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        const TypeId fn_type = typeOfDecl(decl.id);
        const auto *fn       = type_table.function(fn_type);
        if (fn == nullptr)
            continue;
        bool saw_default = false;
        for (size_t index = 0; index < decl.parameters.size(); ++index) {
            const auto &param = decl.parameters[index];
            if (!param.defaultValue) {
                if (saw_default) {
                    report(param.span,
                           "parameter without a default cannot follow a parameter with a default",
                           diagnostics::err::TypeMismatch);
                }
                continue;
            }
            saw_default = true;
            if (index >= fn->params.size())
                continue;
            const TypeId value_type = typeOfExpr(param.defaultValue);
            if (value_type && !coerceValue(param.defaultValue, fn->params[index], value_type)) {
                reportCoercionFailure(param.span, fn->params[index], value_type,
                                      "parameter default type mismatch for '" + param.name + "'");
            }
        }
    }
}
bool PerModuleSema::missingArgsHaveDefaults(const frontend::Declaration &decl, size_t explicit_args,
                                            size_t receiver_offset,
                                            size_t slice_index) const noexcept {
    for (size_t index = receiver_offset + explicit_args; index < decl.parameters.size(); ++index) {
        if (slice_index != ~static_cast<size_t>(0) && index >= slice_index)
            break;
        if (!decl.parameters[index].defaultValue)
            return false;
    }
    return true;
}
TypeId PerModuleSema::functionDefaultType(const frontend::Declaration &decl, size_t param_index,
                                          const PerModuleSema &decl_sema) noexcept {
    if (param_index >= decl.parameters.size() || !decl.parameters[param_index].defaultValue)
        return kInvalidTypeId;
    if (decl.parameters[param_index].defaultValue.value > decl_sema.snapshot.expressions().size())
        return kInvalidTypeId;
    return decl_sema.typeOfExpr(decl.parameters[param_index].defaultValue);
}

} // namespace zith::sema::modern
