#include "sema/hir-lower-modern.hpp"

#include "cinterop/c-header.hpp"
#include "common/overloaded.hpp"
#include "sema/hir-lower-utils.hpp"
#include "sema/op-mapping.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

uint32_t HirLowerModern::alignUp(uint32_t value, uint32_t align) noexcept {
    if (align == 0)
        return value;
    const uint32_t remainder = value % align;
    return remainder == 0 ? value : value + (align - remainder);
}

uint32_t HirLowerModern::lowerTypeSize(types::TypeId type) noexcept {
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
        return 1;
    case types::TypeKind::Int: {
        const auto *integer = std::get_if<types::TypeInt>(&types_.lookup(type));
        return integer != nullptr ? (types::intWidthBits(integer->width) + 7U) / 8U : 0U;
    }
    case types::TypeKind::Float: {
        const auto *floating = std::get_if<types::TypeFloat>(&types_.lookup(type));
        if (floating == nullptr)
            return 0U;
        switch (floating->width) {
        case types::FloatWidth::F32:
            return 4U;
        case types::FloatWidth::F64:
            return 8U;
        case types::FloatWidth::F128:
            return 16U;
        }
        return 0U;
    }
    case types::TypeKind::Ptr:
        return 8U;
    case types::TypeKind::Optional: {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(type));
        if (optional == nullptr)
            return 0U;
        if (types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return 8U;
        const auto inner_size  = lowerTypeSize(optional->inner);
        const auto inner_align = lowerTypeAlign(optional->inner);
        return inner_size == 0U ? 0U : alignUp(alignUp(inner_size, 1U) + 1U, inner_align);
    }
    case types::TypeKind::Failable:
        return 8U;
    case types::TypeKind::Array: {
        const auto *array = std::get_if<types::TypeArray>(&types_.lookup(type));
        return array != nullptr ? lowerTypeSize(array->elem) * array->count : 0U;
    }
    case types::TypeKind::Slice:
        return 16U;
    case types::TypeKind::Enum: {
        const auto *enumeration = std::get_if<types::TypeEnum>(&types_.lookup(type));
        return enumeration != nullptr
                   ? lowerTypeSize(types_.getEnumDef(enumeration->def_id).underlying)
                   : 0U;
    }
    case types::TypeKind::Union: {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(type));
        if (union_type == nullptr)
            return 0U;
        const auto *def = types_.lookupUnionDef(union_type->def_id);
        if (def == nullptr)
            return 0U;
        uint32_t max_bytes = 1U;
        uint32_t max_align = 1U;
        for (const auto member : def->members) {
            max_align = std::max(max_align, lowerTypeAlign(member));
            max_bytes = std::max(max_bytes, lowerTypeSize(member));
        }
        if (!def->is_tagged)
            return alignUp(max_bytes, max_align);
        // Tagged unions append the smallest sufficient member-index tag after
        // the aligned payload.
        const auto payload_bytes = alignUp(max_bytes, max_align);
        return alignUp(payload_bytes + tagByteCount(static_cast<uint32_t>(def->members.size())),
                       max_align);
    }
    case types::TypeKind::Struct: {
        const auto *structure = std::get_if<types::TypeStruct>(&types_.lookup(type));
        if (structure == nullptr)
            return 0U;
        const auto &def = types_.getStructDef(structure->def_id);
        uint32_t offset = 0U;
        for (const auto &field : def.fields) {
            const auto align = lowerTypeAlign(field.type);
            if (align == 0U)
                continue;
            offset = alignUp(offset, align);
            offset += lowerTypeSize(field.type);
        }
        return offset;
    }
    case types::TypeKind::Qualified: {
        const auto *qualified = std::get_if<types::TypeQualified>(&types_.lookup(type));
        return qualified != nullptr ? lowerTypeSize(qualified->inner) : 0U;
    }
    case types::TypeKind::Alias: {
        const auto *alias = std::get_if<types::TypeAlias>(&types_.lookup(type));
        return alias != nullptr ? lowerTypeSize(alias->target) : 0U;
    }
    case types::TypeKind::Nominal: {
        const auto *nominal = std::get_if<types::TypeNominal>(&types_.lookup(type));
        return nominal != nullptr ? lowerTypeSize(nominal->target) : 0U;
    }
    default:
        return 0U;
    }
}

uint32_t HirLowerModern::lowerTypeAlign(types::TypeId type) noexcept {
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
        return 1U;
    case types::TypeKind::Int: {
        const auto *integer = std::get_if<types::TypeInt>(&types_.lookup(type));
        return integer != nullptr ? ((types::intWidthBits(integer->width) + 7U) / 8U) : 0U;
    }
    case types::TypeKind::Float: {
        const auto *floating = std::get_if<types::TypeFloat>(&types_.lookup(type));
        if (floating == nullptr)
            return 0U;
        switch (floating->width) {
        case types::FloatWidth::F32:
            return 4U;
        case types::FloatWidth::F64:
            return 8U;
        case types::FloatWidth::F128:
            return 16U;
        }
        return 0U;
    }
    case types::TypeKind::Ptr:
    case types::TypeKind::Failable:
        return 8U;
    case types::TypeKind::Optional: {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(type));
        if (optional == nullptr)
            return 0U;
        if (types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return 8U;
        return lowerTypeAlign(optional->inner);
    }
    case types::TypeKind::Array: {
        const auto *array = std::get_if<types::TypeArray>(&types_.lookup(type));
        return array != nullptr ? lowerTypeAlign(array->elem) : 0U;
    }
    case types::TypeKind::Slice:
        return 8U;
    case types::TypeKind::Enum: {
        const auto *enumeration = std::get_if<types::TypeEnum>(&types_.lookup(type));
        return enumeration != nullptr
                   ? lowerTypeAlign(types_.getEnumDef(enumeration->def_id).underlying)
                   : 0U;
    }
    case types::TypeKind::Union: {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(type));
        if (union_type == nullptr)
            return 0U;
        const auto *def = types_.lookupUnionDef(union_type->def_id);
        if (def == nullptr)
            return 0U;
        uint32_t max_align = 1U;
        for (const auto member : def->members)
            max_align = std::max(max_align, lowerTypeAlign(member));
        return max_align;
    }
    case types::TypeKind::Struct: {
        const auto *structure = std::get_if<types::TypeStruct>(&types_.lookup(type));
        if (structure == nullptr)
            return 0U;
        uint32_t max_align = 1U;
        for (const auto &field : types_.getStructDef(structure->def_id).fields)
            max_align = std::max(max_align, lowerTypeAlign(field.type));
        return max_align;
    }
    case types::TypeKind::Qualified: {
        const auto *qualified = std::get_if<types::TypeQualified>(&types_.lookup(type));
        return qualified != nullptr ? lowerTypeAlign(qualified->inner) : 0U;
    }
    case types::TypeKind::Alias: {
        const auto *alias = std::get_if<types::TypeAlias>(&types_.lookup(type));
        return alias != nullptr ? lowerTypeAlign(alias->target) : 0U;
    }
    case types::TypeKind::Nominal: {
        const auto *nominal = std::get_if<types::TypeNominal>(&types_.lookup(type));
        return nominal != nullptr ? lowerTypeAlign(nominal->target) : 0U;
    }
    default:
        return 0U;
    }
}

uint32_t HirLowerModern::tagByteCount(uint32_t member_count) noexcept {
    if (member_count <= 0xFFU)
        return 1U;
    if (member_count <= 0xFFFFU)
        return 2U;
    return 4U;
}

types::TypeId HirLowerModern::tagType(types::TypeIntern &types, uint32_t member_count) noexcept {
    if (member_count <= 0xFFU)
        return types.internInt(types::IntWidth::U8);
    if (member_count <= 0xFFFFU)
        return types.internInt(types::IntWidth::U16);
    return types.internInt(types::IntWidth::U32);
}

types::TypeId HirLowerModern::lowerTagType(types::TypeId type, types::TypeIntern &types,
                                           uint32_t member_count) noexcept {
    const auto *union_type = std::get_if<types::TypeUnion>(&types.lookup(type));
    if (union_type == nullptr)
        return types::kInvalidType;
    const auto *def = types.lookupUnionDef(union_type->def_id);
    if (def == nullptr || !def->is_tagged)
        return types::kInvalidType;
    return tagType(types, member_count);
}

uint32_t HirLowerModern::taggedMemberIndex(types::TypeId union_type,
                                           types::TypeId member) noexcept {
    if (types_.kindOf(union_type) != types::TypeKind::Union)
        return ~0U;
    const auto *union_data = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
    if (union_data == nullptr)
        return ~0U;
    const auto *def = types_.lookupUnionDef(union_data->def_id);
    if (def == nullptr || !def->is_tagged)
        return ~0U;
    uint32_t index = 0;
    for (const auto candidate : def->members) {
        if (candidate == member)
            return index;
        ++index;
    }
    return ~0U;
}

hir::HirExprId HirLowerModern::rebuildTaggedUnion(types::TypeId union_type, hir::HirExprId value,
                                                  uint32_t member_index) {
    const auto *union_data = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
    if (union_data == nullptr)
        return hir::kInvalidHirExpr;
    const auto *def = types_.lookupUnionDef(union_data->def_id);
    if (def == nullptr || !def->is_tagged)
        return hir::kInvalidHirExpr;
    const auto &members = def->members;
    hir::HirUnionCast cast;
    cast.value        = value;
    cast.from         = member_index < members.size() ? members[member_index] : types::kInvalidType;
    cast.to           = union_type;
    cast.member_index = member_index;
    cast.checked      = false;
    return addExpr(std::move(cast));
}

types::TypeId HirLowerModern::lowerType(sema::modern::TypeId type) {
    if (!type)
        return types::kErrorType;
    // Nominal placeholders must lower to the completed type, not to Unknown.
    type = sema_.typeTable().canonical(type);
    if (const auto *cached = lowered_types_.get(type.intern_seq))
        return *cached;

    types::TypeId lowered = types::kErrorType;
    switch (sema_.typeTable().kindOf(type)) {
    case TypeKind::Error:
    case TypeKind::Invalid:
        lowered = types::kErrorType;
        break;
    case TypeKind::Void:
        lowered = types::kVoidType;
        break;
    case TypeKind::Never:
        lowered = types::kNeverType;
        break;
    case TypeKind::Bool:
        lowered = types::kBoolType;
        break;
    case TypeKind::Char:
        lowered = types::kCharType;
        break;
    case TypeKind::Integer: {
        const auto *integer = sema_.typeTable().integer(type);
        lowered             = integer != nullptr
                                  ? types_.internInt(sema::mapIntegerWidth(integer->bits, integer->isSigned))
                                  : types::kErrorType;
        break;
    }
    case TypeKind::Float: {
        const auto *floating = sema_.typeTable().float_kind(type);
        lowered = floating != nullptr ? types_.internFloat(sema::mapFloatWidth(floating->bits))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::String:
        lowered = types_.internPtr(types::kCharType);
        break;
    case TypeKind::Pointer: {
        const auto *pointer = sema_.typeTable().pointer(type);
        lowered =
            pointer != nullptr ? types_.internPtr(lowerType(pointer->pointee)) : types::kErrorType;
        break;
    }
    case TypeKind::Optional: {
        const auto *optional = sema_.typeTable().optional(type);
        lowered = optional != nullptr ? types_.internOptional(lowerType(optional->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Array: {
        const auto *array = sema_.typeTable().array(type);
        lowered           = array != nullptr ? types_.internArray(lowerType(array->element),
                                                                  static_cast<uint32_t>(array->size))
                                             : types::kErrorType;
        break;
    }
    case TypeKind::Function:
    case TypeKind::State: {
        const auto *fn = sema_.typeTable().function(type);
        if (fn == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> params(arena_);
        params.reserve(fn->params.size());
        for (const auto param : fn->params)
            params.push(lowerType(param));
        lowered = types_.internFn(params, lowerType(fn->result));
        break;
    }
    case TypeKind::Struct: {
        const auto *structure = sema_.typeTable().struct_type(type);
        if (structure == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the name (done by the named-type not found path).
        lowered = types_.registerNamedType(structure->name, types::TypeKind::Struct);
        // Register the name (done above) before lowering field types so self-referential
        // structs (`next: *Node`) terminate. Fields are copied once, on first lowering.
        if (types_.fieldCount(lowered) == 0U && structure->fields.size() != 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            for (size_t index = 0; index < structure->fields.size(); ++index) {
                const auto field_name = index < structure->field_names.size()
                                            ? structure->field_names[index]
                                            : std::string_view{};
                types_.addField(lowered, field_name, lowerType(structure->fields[index]));
            }
        }
        break;
    }
    case TypeKind::Enum: {
        const auto *enumeration = sema_.typeTable().enum_type(type);
        if (enumeration == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the named enum with its underlying type and variants so codegen can
        // lower it to the underlying integer instead of `void` (a plain registerNamedType
        // would leave the underlying as kErrorType).
        lowered = types_.defineEnum(enumeration->name, lowerType(enumeration->underlying));
        for (size_t i = 0; i < enumeration->variant_names.size(); ++i)
            types_.addEnumVariant(lowered, enumeration->variant_names[i],
                                  enumeration->discriminants[i]);
        break;
    }
    case TypeKind::Union: {
        const auto *union_type = sema_.typeTable().union_type(type);
        if (union_type == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        lowered                   = types_.defineUnion(union_type->name, union_type->is_tagged);
        const auto *lowered_union = std::get_if<types::TypeUnion>(&types_.lookup(lowered));
        const auto *def =
            lowered_union != nullptr ? types_.lookupUnionDef(lowered_union->def_id) : nullptr;
        if (def != nullptr && def->members.size() == 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            for (const auto member : union_type->members)
                types_.addUnionMember(lowered, lowerType(member));
        }
        break;
    }
    case TypeKind::Trait:
    case TypeKind::TypeVar:
    case TypeKind::Unknown:
        lowered = types_.internUnknown();
        break;
    case TypeKind::GenericParam: {
        uint32_t decl_id   = 0;
        uint32_t param_idx = 0;
        sema_.typeTable().genericParamOrigin(type, &decl_id, &param_idx);
        lowered = types_.internGenericParam(decl_id, param_idx);
        break;
    }
    case TypeKind::Incomplete: {
        const auto *incomplete = sema_.typeTable().incomplete(type);
        if (incomplete == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> args(arena_);
        args.reserve(incomplete->args.size());
        for (const auto arg : incomplete->args)
            args.push(lowerType(arg));
        lowered = types_.internIncomplete(lowerType(incomplete->base), args);
        break;
    }
    case TypeKind::Sum: {
        const auto *sum = sema_.typeTable().sum(type);
        if (sum == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> members(arena_);
        members.reserve(sum->members.size());
        for (const auto member : sum->members)
            members.push(lowerType(member));
        lowered = types_.internSum(members);
        break;
    }
    case TypeKind::Slice: {
        const auto *slice = sema_.typeTable().slice(type);
        lowered =
            slice != nullptr ? types_.internSlice(lowerType(slice->element)) : types::kErrorType;
        break;
    }
    case TypeKind::Failable: {
        const auto *failable = sema_.typeTable().failable(type);
        lowered = failable != nullptr ? types_.internFailable(lowerType(failable->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Pack: {
        const auto *pack = sema_.typeTable().pack(type);
        if (pack == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        memory::DynArray<types::TypeId> members(arena_);
        memory::DynArray<memory::InternedId> names(arena_);
        members.reserve(pack->members.size());
        names.reserve(pack->names.size());
        for (const auto member : pack->members)
            members.push(lowerType(member));
        for (const auto name : pack->names)
            names.push(interner_.intern(name));
        lowered = types_.internPack(members, names);
        break;
    }
    case TypeKind::Dyn: {
        const auto *dyn = sema_.typeTable().dyn_type(type);
        lowered = dyn != nullptr ? types_.internDyn(lowerType(dyn->target), dyn->method_count)
                                 : types::kErrorType;
        break;
    }
    case TypeKind::Opaque: {
        lowered = types_.internOpaqueTagged();
        break;
    }
    case TypeKind::Alias: {
        const auto *alias = sema_.typeTable().alias(type);
        lowered           = alias != nullptr ? lowerType(alias->target) : types::kErrorType;
        break;
    }
    case TypeKind::Nominal: {
        const auto *nom = sema_.typeTable().nominal(type);
        if (nom == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        lowered = types_.defineStruct(nom->name);
        if (types_.fieldCount(lowered) == 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            types_.addField(lowered, "", lowerType(nom->target));
        }
        break;
    }
    case TypeKind::Qualified: {
        // HIR and codegen do not represent ownership: strip to the inner type.
        const auto *qual = sema_.typeTable().qualified(type);
        lowered          = qual != nullptr ? lowerType(qual->inner) : types::kErrorType;
        break;
    }
    }

    lowered_types_.insert(type.intern_seq, lowered);
    return lowered;
}

sema::modern::TypeId HirLowerModern::lowerTypeExprConcrete(frontend::TypeExprId id) {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr)
        return sema::modern::kInvalidTypeId;
    sema::modern::TypeId lowered = sema_.typeTable().lowerTypeExpr(*current_module_->frontend, id);
    if (!lowered && current_fn_decl_ != nullptr &&
        id.value <= current_module_->frontend->typeExpressions().size()) {
        const auto &type_expr = current_module_->frontend->typeExpressions()[id.value - 1U];
        if (type_expr.kind == frontend::TypeExprKind::Name && type_expr.arguments.empty()) {
            const auto findGenericParam = [&](const frontend::Declaration &decl) {
                for (size_t i = 0; i < decl.genericParams.size(); ++i) {
                    if (decl.genericParams[i].name == type_expr.name)
                        return sema_.typeTable().internGenericParam(decl.id.value,
                                                                    static_cast<uint32_t>(i));
                }
                return sema::modern::kInvalidTypeId;
            };
            if (!current_fn_decl_->ownerName.empty()) {
                for (const auto &decl : current_module_->frontend->declarations()) {
                    if (decl.name == current_fn_decl_->ownerName &&
                        decl.id.value != current_fn_decl_->id.value) {
                        lowered = findGenericParam(decl);
                        break;
                    }
                }
            }
            if (!lowered)
                lowered = findGenericParam(*current_fn_decl_);
        }
    }
    if (lowered && current_instantiation_ != nullptr && current_instance_ != nullptr) {
        lowered = current_instantiation_->substituteType(lowered, current_instance_->args);
    }
    return lowered;
}

types::TypeId HirLowerModern::lowerForeignType(const cinterop::Type &type) {
    switch (type.kind) {
    case cinterop::TypeKind::Void:
        return types::kVoidType;
    case cinterop::TypeKind::Bool:
        return types::kBoolType;
    case cinterop::TypeKind::Integer:
        if (type.isChar)
            return types::kCharType;
        return types_.internInt(sema::mapIntegerWidth(type.bits, type.isSigned));
    case cinterop::TypeKind::Float:
        return types_.internFloat(sema::mapFloatWidth(type.bits));
    case cinterop::TypeKind::Pointer: {
        // Mirrors `PerModuleSema::lowerForeignType`: a C pointer is `?*T`, which the
        // niche layout emits as the bare pointer.
        const types::TypeId pointee =
            type.pointee ? lowerForeignType(*type.pointee) : types::kErrorType;
        return types_.internOptional(types_.internPtr(pointee));
    }
    case cinterop::TypeKind::Record:
        return types_.registerNamedType(type.name, types::TypeKind::Struct);
    case cinterop::TypeKind::Enum:
        return types_.registerNamedType(type.name, types::TypeKind::Enum);
    }
    return types::kErrorType;
}

types::TypeId HirLowerModern::typeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->exprTypes.get(id.value);
    if (type == nullptr)
        return types::kErrorType;
    const sema::modern::TypeId sema_type =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*type, current_instance_->args)
            : *type;
    return lowerType(sema_type);
}

types::TypeId HirLowerModern::typeOfLocal(frontend::LocalId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->localTypes.get(id.value);
    if (type == nullptr)
        return types::kErrorType;
    const sema::modern::TypeId sema_type =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*type, current_instance_->args)
            : *type;
    return lowerType(sema_type);
}

sema::modern::TypeId HirLowerModern::semaTypeOfLocal(frontend::LocalId id) {
    if (!id || current_types_ == nullptr)
        return kInvalidTypeId;
    const auto *type = current_types_->localTypes.get(id.value);
    if (type == nullptr)
        return kInvalidTypeId;
    return current_instantiation_ != nullptr && current_instance_ != nullptr
               ? current_instantiation_->substituteType(*type, current_instance_->args)
               : *type;
}

sema::modern::TypeId HirLowerModern::semaTypeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return kInvalidTypeId;
    const auto *sema_id_ptr = current_types_->exprTypes.get(id.value);
    if (!sema_id_ptr)
        return kInvalidTypeId;
    return current_instantiation_ != nullptr && current_instance_ != nullptr
               ? current_instantiation_->substituteType(*sema_id_ptr, current_instance_->args)
               : *sema_id_ptr;
}

uint32_t HirLowerModern::stableConcreteTypeId(types::TypeId type) const {
    uint32_t hash     = 2166136261U;
    const auto append = [&](const uint8_t *bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= 16777619U;
        }
    };
    auto appendU64 = [&](uint64_t value) {
        const uint8_t raw[sizeof(value)] = {
            static_cast<uint8_t>(value),        static_cast<uint8_t>(value >> 8U),
            static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U),
            static_cast<uint8_t>(value >> 32U), static_cast<uint8_t>(value >> 40U),
            static_cast<uint8_t>(value >> 48U), static_cast<uint8_t>(value >> 56U)};
        append(raw, sizeof(raw));
    };
    const auto appendName = [&](memory::InternedId name) {
        const auto text = interner_.lookup(name);
        append(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    };

    const auto namespace_text =
        moduleNamespace(current_module_ != nullptr ? current_module_->key : std::string_view{},
                        snapshot_.cacheKey());
    append(reinterpret_cast<const uint8_t *>(namespace_text.data()), namespace_text.size());
    appendU64(static_cast<uint64_t>(static_cast<TypeKind>(types_.kindOf(type))));

    auto appendType = [&](const auto &self, types::TypeId current) -> void {
        const auto &data = types_.lookup(current);
        std::visit(common::overloaded{
                       [&](const types::TypeError &) { appendU64(1); },
                       [&](const types::TypeNever &) { appendU64(2); },
                       [&](const types::TypeVoid &) { appendU64(3); },
                       [&](const types::TypeBool &) { appendU64(4); },
                       [&](const types::TypeChar &) { appendU64(5); },
                       [&](const types::TypeInt &t) {
                           appendU64(6);
                           appendU64(static_cast<uint64_t>(t.width));
                       },
                       [&](const types::TypeFloat &t) {
                           appendU64(7);
                           appendU64(static_cast<uint64_t>(t.width));
                       },
                       [&](const types::TypePtr &t) {
                           appendU64(8);
                           appendU64(static_cast<uint64_t>(t.is_mut));
                           appendU64(static_cast<uint64_t>(t.ownership));
                           self(self, t.pointee);
                       },
                       [&](const types::TypeArray &t) {
                           appendU64(9);
                           appendU64(t.count);
                           self(self, t.elem);
                       },
                       [&](const types::TypeStruct &) {
                           appendU64(10);
                           const auto &def = types_.getStructDef(current);
                           appendName(def.name);
                           for (const auto &field : def.fields) {
                               appendName(field.name);
                               self(self, field.type);
                           }
                       },
                       [&](const types::TypeFn &t) {
                           appendU64(11);
                           appendU64(t.param_count);
                           self(self, t.ret);
                           for (size_t i = 0; i < t.param_count; ++i)
                               self(self, t.params[i]);
                       },
                       [&](const types::TypeTypeVar &t) {
                           appendU64(12);
                           appendU64(t.id);
                       },
                       [&](const types::TypeOptional &t) {
                           appendU64(13);
                           self(self, t.inner);
                       },
                       [&](const types::TypeFailable &t) {
                           appendU64(14);
                           self(self, t.inner);
                       },
                       [&](const types::TypeAlias &t) {
                           appendU64(15);
                           self(self, t.target);
                       },
                       [&](const types::TypeNominal &t) {
                           appendU64(16);
                           appendName(t.name);
                           self(self, t.target);
                       },
                       [&](const types::TypeTrait &t) {
                           appendU64(17);
                           appendName(t.name);
                       },
                       [&](const types::TypeDyn &t) {
                           appendU64(18);
                           appendU64(t.method_count);
                           self(self, t.target);
                       },
                       [&](const types::TypeOpaque &) { appendU64(19); },
                       [&](const types::TypeOpaqueTagged &) { appendU64(20); },
                       [&](const types::TypeUnknown &) { appendU64(21); },
                       [&](const types::TypeQualified &t) {
                           appendU64(22);
                           appendU64(static_cast<uint64_t>(t.ownership));
                           appendU64(static_cast<uint64_t>(t.isMut));
                           self(self, t.inner);
                       },
                       [&](const types::TypeSlice &t) {
                           appendU64(23);
                           self(self, t.elem);
                       },
                       [&](const types::TypeEnum &) {
                           appendU64(24);
                           const auto &def = types_.getEnumDef(current);
                           appendName(def.name);
                           self(self, def.underlying);
                           for (const auto &variant : def.variants) {
                               appendName(variant.name);
                               appendU64(static_cast<uint64_t>(variant.discriminant));
                           }
                       },
                       [&](const types::TypeUnion &) {
                           appendU64(25);
                           const auto &def = types_.getUnionDef(current);
                           appendU64(static_cast<uint64_t>(def.is_tagged));
                           appendName(def.name);
                           for (const auto &member : def.members)
                               self(self, member);
                       },
                       [&](const types::TypePack &t) {
                           appendU64(26);
                           appendU64(t.count);
                           for (size_t i = 0; i < t.count; ++i)
                               self(self, t.members[i]);
                       },
                       [&](const types::TypeSum &t) {
                           appendU64(27);
                           appendU64(t.count);
                           for (size_t i = 0; i < t.count; ++i)
                               self(self, t.members[i]);
                       },
                       [&](const types::TypeGenericParam &t) {
                           appendU64(28);
                           appendU64(t.decl_id);
                           appendU64(t.param_index);
                       },
                       [&](const types::TypeIncomplete &t) {
                           appendU64(29);
                           appendU64(t.arg_count);
                           self(self, t.base);
                           for (size_t i = 0; i < t.arg_count; ++i)
                               self(self, t.args[i]);
                       },
                   },
                   data);
    };
    appendType(appendType, type);

    const auto domain =
        moduleNamespace(current_module_ != nullptr ? current_module_->key : std::string_view{},
                        snapshot_.cacheKey());
    append(reinterpret_cast<const uint8_t *>(domain.data()), domain.size());

    return hash == 0U ? 1U : hash;
}

/// `x is null` lowers to a tag/pointer comparison; no dedicated HIR node is needed.

} // namespace modern
} // namespace zith::sema
