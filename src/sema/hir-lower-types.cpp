#include "sema/hir-lower-modern.hpp"

#include "cache/cache.hpp"
#include "cinterop/c-header.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "sema/op-mapping.hpp"
#include "types/type-canonical.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

uint32_t HirLowerModern::lowerTypeSize(types::TypeId type) const noexcept {
    return types::typeByteCount(types_, type);
}

uint32_t HirLowerModern::lowerTypeAlign(types::TypeId type) const noexcept {
    return types::typeAlignBytes(types_, type);
}

uint32_t HirLowerModern::tagByteCount(uint32_t member_count) noexcept {
    return types::tagByteCount(member_count);
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
    cast.safety       = hir::HirSafetyMode::Intrinsic;
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
        if (pointer == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        const auto pointee = sema_.typeTable().canonical(pointer->pointee);
        if (sema_.typeTable().kindOf(pointee) == TypeKind::Enum) {
            const std::string_view name = sema_.typeTable().namedTypeName(pointee);
            const auto *foreign         = foreignEnumForName(name);
            if (foreign != nullptr && foreign->enumUnderlying == nullptr) {
                // An incomplete foreign enum is valid as an opaque pointer
                // target, but must not be cached as a by-value type.
                const auto opaque = types_.registerNamedType(name, types::TypeKind::Enum);
                types_.setDefiningModule(opaque, sema_.typeTable().definingModule(pointee));
                lowered = types_.internPtr(opaque);
                break;
            }
        }
        lowered = types_.internPtr(lowerType(pointer->pointee));
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
        if (structure != nullptr) {
            // Register the name (done by the named-type not found path).
            lowered = types_.registerNamedType(structure->name, types::TypeKind::Struct);
            types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
            types_.setExplicitAlignment(lowered, structure->explicit_alignment);
            if (!structure->args.empty()) {
                memory::DynArray<types::TypeId> lowered_args(arena_);
                lowered_args.reserve(structure->args.size());
                for (const auto arg : structure->args)
                    lowered_args.push(lowerType(arg));
                types_.setTypeArgs(lowered, lowered_args);
            }
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
        const std::string_view name = sema_.typeTable().namedTypeName(type);
        if (const auto *foreign = foreignRecordForName(name); foreign != nullptr) {
            lowered = types_.registerNamedType(name, types::TypeKind::Struct);
            types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
            if (types_.fieldCount(lowered) == 0U && foreign->hasVerifiedLayout) {
                lowered_types_.insert(type.intern_seq, lowered);
                types_.setForeignLayout(lowered, foreign->sizeBytes, foreign->alignBytes,
                                        foreign->abiIsSingleI64);
                fillForeignStruct(lowered, *foreign);
            }
            break;
        }
        lowered = types::kErrorType;
        break;
    }
    case TypeKind::Enum: {
        const std::string_view name = sema_.typeTable().namedTypeName(type);
        if (const auto *foreign = foreignEnumForName(name); foreign != nullptr) {
            lowered = lowerForeignType(*foreign);
            if (lowered != types::kErrorType)
                types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
            break;
        }
        const auto *enumeration = sema_.typeTable().enum_type(type);
        if (enumeration == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the named enum with its underlying type and variants so codegen can
        // lower it to the underlying integer instead of `void` (a plain registerNamedType
        // would leave the underlying as kErrorType).
        lowered = types_.defineEnum(enumeration->name, lowerType(enumeration->underlying));
        types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
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
        lowered = types_.defineUnion(union_type->name, union_type->is_tagged);
        types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
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
    case TypeKind::Trait: {
        const auto *trait = sema_.typeTable().trait(type);
        lowered = trait != nullptr ? types_.internTrait(trait->name) : types::kErrorType;
        break;
    }
    case TypeKind::TypeVar:
    case TypeKind::Unknown:
        lowered = types_.internUnknown();
        break;
    case TypeKind::GenericParam: {
        uint32_t decl_id   = 0;
        uint32_t param_idx = 0;
        sema_.typeTable().genericParamOrigin(type, &decl_id, &param_idx);
        if (current_module_ == nullptr || current_module_->frontend == nullptr ||
            current_module_->source == nullptr || decl_id == 0U ||
            decl_id > current_module_->frontend->declarations().size()) {
            lowered = types::kErrorType;
            break;
        }
        const auto &owner = current_module_->frontend->declarations()[decl_id - 1U];
        std::string owner_identity = current_module_->source->canonicalPath + "::";
        if (!owner.ownerName.empty())
            owner_identity += owner.ownerName + ".";
        if (!owner.parentName.empty())
            owner_identity += owner.parentName + "$local.";
        owner_identity += owner.name;
        if (owner.kind == frontend::DeclKind::Function)
            owner_identity += frontend::functionSignature(*current_module_->frontend, owner);
        lowered = types_.internGenericParam(owner_identity, param_idx);
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
        types_.setDefiningModule(lowered, sema_.typeTable().definingModule(type));
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
    sema::modern::TypeId lowered = sema::modern::kInvalidTypeId;
    // Qualified names (`mod.Type`) resolve through import bindings, which the
    // raw TypeTable lowering cannot see; defer to the module sema for those.
    if (id.value <= current_module_->frontend->typeExpressions().size()) {
        const auto &type_expr = current_module_->frontend->typeExpressions()[id.value - 1U];
        if (type_expr.kind == frontend::TypeExprKind::Name && !type_expr.segments.empty()) {
            if (PerModuleSema *module_sema = sema_.findModuleSema(current_module_->key))
                lowered = module_sema->lowerTypeExpr(id);
        }
    }
    if (!lowered)
        lowered = sema_.typeTable().lowerTypeExpr(*current_module_->frontend, id);
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
            type.pointee ? lowerForeignPointee(*type.pointee) : types::kErrorType;
        return types_.internOptional(types_.internPtr(pointee));
    }
    case cinterop::TypeKind::Record: {
        if (!type.hasVerifiedLayout) {
            diags_.reportError(diagnostics::err::InvalidIR,
                               "unsupported C ABI: record '" + type.name +
                                   "' is used by value without a verified layout",
                               {});
            return types::kErrorType;
        }
        const types::TypeId lowered = types_.registerNamedType(type.name, types::TypeKind::Struct);
        if (types_.kindOf(lowered) == types::TypeKind::Struct && types_.fieldCount(lowered) == 0U &&
            type.hasVerifiedLayout) {
            types_.setForeignLayout(lowered, type.sizeBytes, type.alignBytes, type.abiIsSingleI64);
            fillForeignStruct(lowered, type);
        }
        return lowered;
    }
    case cinterop::TypeKind::Enum: {
        // A C enum value is carried by its underlying integer type.  Without it
        // the enum would lower to `void` and the LLVM verifier would reject the
        // declaration (or the backend would loop).  Reject an enum with no known
        // underlying instead of emitting an unproven ABI.
        if (type.enumUnderlying == nullptr) {
            diags_.reportError(diagnostics::err::InvalidIR,
                               "unsupported C ABI: enum '" + type.name +
                                   "' is used by value without a known underlying type",
                               {});
            return types::kErrorType;
        }
        const types::TypeId underlying = lowerForeignType(*type.enumUnderlying);
        const types::TypeId lowered    = types_.defineEnum(type.name, underlying);
        types_.setEnumUnderlying(lowered, underlying);
        return lowered;
    }
    }
    return types::kErrorType;
}

const cinterop::Type *HirLowerModern::foreignRecordForName(std::string_view name) const noexcept {
    const auto *cached = foreign_record_types_.get(interner_.intern(name));
    return cached != nullptr ? *cached : nullptr;
}

const cinterop::Type *HirLowerModern::foreignEnumForName(std::string_view name) const noexcept {
    const auto *cached = foreign_enum_types_.get(interner_.intern(name));
    return cached != nullptr ? *cached : nullptr;
}

void HirLowerModern::fillForeignStruct(types::TypeId lowered, const cinterop::Type &record) {
    for (const auto &field : record.recordFields) {
        if (field.type == nullptr)
            continue;
        const types::TypeId field_type = lowerForeignType(*field.type);
        if (field_type == types::kErrorType)
            continue;
        types_.addField(lowered, field.name, field_type, field.offsetBits / 8U);
    }
}

types::TypeId HirLowerModern::lowerForeignPointee(const cinterop::Type &type) {
    if (type.kind == cinterop::TypeKind::Record) {
        // A pointer to a C record has a bare pointer ABI even when the record
        // layout itself is not needed by the caller.
        return types_.registerNamedType(type.name, types::TypeKind::Struct);
    }
    if (type.kind == cinterop::TypeKind::Enum) {
        // A pointer to a C enum has a bare pointer ABI.  The underlying type is
        // not needed for a pointer, so a forward-declared enum stays usable here.
        return types_.registerNamedType(type.name, types::TypeKind::Enum);
    }
    return lowerForeignType(type);
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

types::TypeCanonicalId HirLowerModern::canonicalTypeId(types::TypeId type) const {
    const auto resolve_namespace = [this](std::string_view module_key) {
        return moduleNamespace(module_key, snapshot_.cacheKey());
    };
    return types::canonicalTypeId(types_, type, resolve_namespace);
}

uint32_t
HirLowerModern::runtimeTagForCanonicalType(const types::TypeCanonicalId &canonical_id) const {
    const uint32_t tag = cache_store_ != nullptr ? cache_store_->assignCanonicalId(canonical_id)
                                                 : types_.canonicalTag(canonical_id, 0U);
    types_.setCanonicalTag(canonical_id, tag);
    return tag;
}

/// `x is null` lowers to a tag/pointer comparison; no dedicated HIR node is needed.

} // namespace modern
} // namespace zith::sema
