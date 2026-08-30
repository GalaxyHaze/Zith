#include "sema/modern-types.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace zith::sema::modern {

TypeTable::TypeTable(memory::Arena &arena)
    : entries_(arena), arena_(&arena), named_registry_(), conformances_(arena, this) {}

TypeTable::Entry &TypeTable::pushEntry(EntryKind kind) {
    Entry entry{};
    entry.id            = TypeId{next_seq_++};
    entry.kind          = kind;
    entry.reported_kind = TypeKind::Error;
    entries_.push(entry);
    return entries_.back();
}

memory::DynArray<TypeId> &TypeTable::makeStorage() {
    return *arena_->make<memory::DynArray<TypeId>>(*arena_);
}

memory::DynArray<std::string_view> &TypeTable::makeNameStorage() {
    return *arena_->make<memory::DynArray<std::string_view>>(*arena_);
}

memory::DynArray<FieldMeta> &TypeTable::makeFieldMetaStorage() {
    return *arena_->make<memory::DynArray<FieldMeta>>(*arena_);
}

memory::DynArray<int64_t> &TypeTable::makeDiscStorage() {
    return *arena_->make<memory::DynArray<int64_t>>(*arena_);
}

memory::DynArray<TypeId> &TypeTable::makeTypeStorage() {
    return makeStorage();
}

memory::DynArray<std::string_view> &TypeTable::makeStringStorage() {
    return makeNameStorage();
}

// --- Intern helpers ---

TypeId TypeTable::internName(std::string_view name, TypeKind kind) {
    auto &entry         = pushEntry(EntryKind::Name);
    entry.name_view     = persistString(name);
    entry.reported_kind = kind;
    return entry.id;
}

TypeId TypeTable::internInvalid() {
    auto &entry         = pushEntry(EntryKind::Invalid);
    entry.reported_kind = TypeKind::Invalid;
    return entry.id;
}

TypeId TypeTable::internInteger(IntegerType int_type) {
    auto &entry         = pushEntry(EntryKind::Integer);
    entry.integer       = int_type;
    entry.reported_kind = TypeKind::Integer;
    return entry.id;
}

TypeId TypeTable::internFloat(FloatType float_type) {
    auto &entry         = pushEntry(EntryKind::Float);
    entry.float_ty      = float_type;
    entry.reported_kind = TypeKind::Float;
    return entry.id;
}

TypeId TypeTable::internPointer(TypeId pointee) {
    auto &entry         = pushEntry(EntryKind::Pointer);
    entry.pointer_ty    = PointerType{pointee};
    entry.reported_kind = TypeKind::Pointer;
    return entry.id;
}

TypeId TypeTable::internOptional(TypeId inner) {
    auto &entry         = pushEntry(EntryKind::Optional);
    entry.optional_ty   = OptionalType{inner};
    entry.reported_kind = TypeKind::Optional;
    return entry.id;
}

TypeId TypeTable::internArray(TypeId element, uint64_t size) {
    auto &entry         = pushEntry(EntryKind::Array);
    entry.array_ty      = ArrayType{element, size};
    entry.reported_kind = TypeKind::Array;
    return entry.id;
}

TypeId TypeTable::internFunction(memory::DynArray<TypeId> &params, TypeId result) {
    auto &entry         = pushEntry(EntryKind::Function);
    entry.reported_kind = TypeKind::Function;
    auto &storage       = makeStorage();
    for (auto &p : params)
        storage.push(p);
    entry.fn      = arena_->make<FunctionType>(FunctionType{storage, result});
    entry.storage = &storage;
    return entry.id;
}

TypeId TypeTable::internStateFunction(memory::DynArray<TypeId> &params, TypeId result) {
    auto &entry         = pushEntry(EntryKind::StateFunction);
    entry.reported_kind = TypeKind::State;
    auto &storage       = makeStorage();
    for (auto &p : params)
        storage.push(p);
    entry.fn      = arena_->make<FunctionType>(FunctionType{storage, result});
    entry.storage = &storage;
    return entry.id;
}

TypeId TypeTable::internStruct(std::string_view name, memory::DynArray<TypeId> &fields,
                               memory::DynArray<std::string_view> *field_names,
                               memory::DynArray<FieldMeta> *field_meta) {
    auto &entry         = pushEntry(EntryKind::Struct);
    entry.reported_kind = TypeKind::Struct;
    entry.name_view     = persistString(name);
    auto &storage       = makeStorage();
    for (auto &f : fields)
        storage.push(f);
    auto &name_storage = makeNameStorage();
    if (field_names != nullptr) {
        for (auto &n : *field_names)
            name_storage.push(n);
    }
    auto &meta_storage = makeFieldMetaStorage();
    if (field_meta != nullptr) {
        for (auto &meta : *field_meta)
            meta_storage.push(meta);
    }
    entry.struct_ty =
        arena_->make<StructType>(StructType{entry.name_view, storage, name_storage, meta_storage});
    entry.storage      = &storage;
    entry.name_storage = &name_storage;
    entry.meta_storage = &meta_storage;
    return entry.id;
}

int TypeTable::fieldIndex(TypeId struct_type, std::string_view name) const noexcept {
    const auto *entry = findEntry(struct_type);
    if (entry == nullptr)
        return -1;
    const auto *names = entry->struct_ty != nullptr ? &entry->struct_ty->field_names : nullptr;
    if (names == nullptr) {
        if (entry->pack_ty != nullptr)
            names = &entry->pack_ty->names;
    }
    if (names == nullptr)
        return -1;
    const auto &name_list = *names;
    for (size_t i = 0; i < name_list.size(); ++i) {
        if (name_list[i] == name)
            return static_cast<int>(i);
    }
    return -1;
}

TypeId TypeTable::internEnum(std::string_view name, TypeId underlying,
                             memory::DynArray<std::string_view> &variant_names,
                             memory::DynArray<int64_t> &discriminants) {
    auto &entry         = pushEntry(EntryKind::Enum);
    entry.reported_kind = TypeKind::Enum;
    entry.underlying    = underlying;
    auto &name_storage  = makeNameStorage();
    for (auto &n : variant_names)
        name_storage.push(n);
    auto &disc_storage = makeDiscStorage();
    for (auto &d : discriminants)
        disc_storage.push(d);
    entry.enum_ty = arena_->make<EnumType>(EnumType{name, underlying, name_storage, disc_storage});
    entry.name_storage = &name_storage;
    entry.disc_storage = &disc_storage;
    return entry.id;
}

TypeId TypeTable::internUnion(std::string_view name, memory::DynArray<TypeId> &members,
                              bool is_tagged) {
    auto &entry         = pushEntry(EntryKind::Union);
    entry.reported_kind = TypeKind::Union;
    auto &storage       = makeStorage();
    for (auto &m : members)
        storage.push(m);
    entry.union_ty = arena_->make<UnionType>(UnionType{name, storage, is_tagged});
    entry.storage  = &storage;
    return entry.id;
}

TypeId TypeTable::internTrait(std::string_view name) {
    auto &entry         = pushEntry(EntryKind::Trait);
    entry.reported_kind = TypeKind::Trait;
    entry.trait_ty      = arena_->make<TraitType>(TraitType{name});
    return entry.id;
}

TypeId TypeTable::internInterface(std::string_view name) {
    auto &entry         = pushEntry(EntryKind::Trait);
    entry.reported_kind = TypeKind::Trait;
    entry.trait_ty      = arena_->make<TraitType>(TraitType{name});
    return entry.id;
}

TypeId TypeTable::internTypeVar() {
    auto &entry         = pushEntry(EntryKind::TypeVar);
    entry.reported_kind = TypeKind::TypeVar;
    entry.var_ty        = arena_->make<TypeVarType>(TypeVarType{next_var_++});
    return entry.id;
}

TypeId TypeTable::internUnknown() {
    auto &entry         = pushEntry(EntryKind::Unknown);
    entry.reported_kind = TypeKind::Unknown;
    return entry.id;
}

TypeId TypeTable::internGenericParam(uint32_t decl_id, uint32_t param_index) {
    auto &entry               = pushEntry(EntryKind::GenericParam);
    entry.reported_kind       = TypeKind::GenericParam;
    entry.generic_decl_id     = decl_id;
    entry.generic_param_index = param_index;
    return entry.id;
}

TypeId TypeTable::internIncomplete(TypeId base, memory::DynArray<TypeId> &args) {
    auto &entry         = pushEntry(EntryKind::Incomplete);
    entry.reported_kind = TypeKind::Incomplete;
    auto &storage       = makeStorage();
    for (auto &a : args)
        storage.push(a);
    entry.incomplete_ty = arena_->make<IncompleteType>(IncompleteType{base, storage});
    entry.storage       = &storage;
    return entry.id;
}

TypeId TypeTable::internSum(memory::DynArray<TypeId> &members) {
    auto &entry         = pushEntry(EntryKind::Sum);
    entry.reported_kind = TypeKind::Sum;
    auto &storage       = makeStorage();
    for (auto &m : members)
        storage.push(m);
    entry.sum_ty  = arena_->make<SumType>(SumType{storage});
    entry.storage = &storage;
    return entry.id;
}

TypeId TypeTable::internSlice(TypeId element) {
    auto &entry         = pushEntry(EntryKind::Slice);
    entry.reported_kind = TypeKind::Slice;
    entry.slice_ty      = SliceType{element};
    return entry.id;
}

TypeId TypeTable::internFailable(TypeId inner) {
    auto &entry         = pushEntry(EntryKind::Failable);
    entry.reported_kind = TypeKind::Failable;
    entry.failable_ty   = FailableType{inner};
    return entry.id;
}

TypeId TypeTable::internPack(memory::DynArray<TypeId> &members,
                             memory::DynArray<std::string_view> &names) {
    auto &entry         = pushEntry(EntryKind::Pack);
    entry.reported_kind = TypeKind::Pack;
    auto &type_storage  = makeStorage();
    for (auto &m : members)
        type_storage.push(m);
    auto &name_storage = makeNameStorage();
    for (auto &n : names)
        name_storage.push(n);
    entry.pack_ty      = arena_->make<PackType>(PackType{type_storage, name_storage});
    entry.storage      = &type_storage;
    entry.name_storage = &name_storage;
    return entry.id;
}

TypeId TypeTable::internDyn(TypeId target, size_t method_count) {
    auto &entry         = pushEntry(EntryKind::Dyn);
    entry.reported_kind = TypeKind::Dyn;
    entry.dyn_ty        = arena_->make<DynType>(DynType{target, method_count});
    return entry.id;
}

TypeId TypeTable::internOpaque() {
    auto &entry         = pushEntry(EntryKind::Opaque);
    entry.reported_kind = TypeKind::Opaque;
    entry.opaque_ty     = {};
    return entry.id;
}

TypeId TypeTable::internAlias(TypeId target) {
    auto &entry         = pushEntry(EntryKind::Alias);
    entry.reported_kind = TypeKind::Alias;
    entry.name_view     = {};
    entry.alias_ty      = arena_->make<AliasType>(AliasType{target});
    return entry.id;
}

TypeId TypeTable::internAlias(std::string_view name, TypeId target) {
    auto &entry         = pushEntry(EntryKind::Alias);
    entry.reported_kind = TypeKind::Alias;
    entry.name_view     = persistString(name);
    entry.alias_ty      = arena_->make<AliasType>(AliasType{target});
    return entry.id;
}

TypeId TypeTable::internNominal(std::string_view name, TypeId target) {
    auto &entry           = pushEntry(EntryKind::Nominal);
    entry.reported_kind   = TypeKind::Nominal;
    auto &storage         = makeStorage();
    const TypeId fields[] = {target};
    for (const auto field : fields)
        storage.push(field);
    entry.name_view  = persistString(name);
    entry.nominal_ty = arena_->make<NominalType>(NominalType{entry.name_view, target, storage});
    entry.storage    = &storage;
    return entry.id;
}

TypeId TypeTable::internQualified(TypeId inner, types::OwnershipKind ownership, bool is_mut) {
    auto &entry         = pushEntry(EntryKind::Qualified);
    entry.reported_kind = TypeKind::Qualified;
    entry.qualified_ty  = arena_->make<QualifiedType>(QualifiedType{inner, ownership, is_mut});
    return entry.id;
}

// --- Queries ---

TypeKind TypeTable::kindOf(TypeId id) const noexcept {
    const auto *entry = findEntry(canonical(id));
    return entry ? entry->reported_kind : TypeKind::Error;
}

std::string TypeTable::typeToString(TypeId id) const {
    const TypeId resolved = canonical(id);
    const Entry *entry    = findEntry(resolved);
    if (entry == nullptr)
        return "void";

    switch (entry->reported_kind) {
    case TypeKind::Invalid:
        return "invalid";
    case TypeKind::Error:
        return "error";
    case TypeKind::Void:
        return "void";
    case TypeKind::Never:
        return "never";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::Char:
        return "char";
    case TypeKind::String:
        return "string";
    case TypeKind::Integer:
        if (const auto *int_type = integer(resolved); int_type != nullptr)
            return std::string(int_type->isSigned ? "i" : "u") + std::to_string(int_type->bits);
        return "integer";
    case TypeKind::Float:
        if (const auto *float_type = float_kind(resolved); float_type != nullptr)
            return "f" + std::to_string(float_type->bits);
        return "float";
    case TypeKind::Pointer:
        if (const auto *ptr = pointer(resolved); ptr != nullptr)
            return "*" + typeToString(ptr->pointee);
        return "pointer";
    case TypeKind::Optional:
        if (const auto *opt = optional(resolved); opt != nullptr)
            return "?" + typeToString(opt->inner);
        return "optional";
    case TypeKind::Slice:
        if (const auto *slice = this->slice(resolved); slice != nullptr)
            return "[]" + typeToString(slice->element);
        return "slice";
    case TypeKind::Array:
        if (const auto *array = this->array(resolved); array != nullptr)
            return "[" + std::to_string(array->size) + "]" + typeToString(array->element);
        return "array";
    case TypeKind::Function:
        if (const auto *fn = function(resolved); fn != nullptr) {
            std::string result = "fn(";
            for (size_t index = 0; index < fn->params.size(); ++index) {
                if (index != 0)
                    result += ", ";
                result += typeToString(fn->params[index]);
            }
            result += "): " + typeToString(fn->result);
            return result;
        }
        return "fn";
    case TypeKind::State:
        if (const auto *fn = function(resolved); fn != nullptr) {
            std::string result = "state(";
            for (size_t index = 0; index < fn->params.size(); ++index) {
                if (index != 0)
                    result += ", ";
                result += typeToString(fn->params[index]);
            }
            result += "): " + typeToString(fn->result);
            return result;
        }
        return "state";
    case TypeKind::Struct:
        if (const auto *st = struct_type(resolved); st != nullptr)
            return std::string(st->name);
        return "struct";
    case TypeKind::Enum:
        if (const auto *et = enum_type(resolved); et != nullptr)
            return std::string(et->name);
        return "enum";
    case TypeKind::Union:
        if (const auto *ut = union_type(resolved); ut != nullptr)
            return std::string(ut->name);
        return "union";
    case TypeKind::Trait:
        if (const auto *tt = trait(resolved); tt != nullptr)
            return std::string(tt->name);
        return "trait";
    case TypeKind::TypeVar:
        if (const auto *var = type_var(resolved); var != nullptr)
            return "type$" + std::to_string(var->id);
        return "typevar";
    case TypeKind::Unknown:
        return "unknown";
    case TypeKind::GenericParam:
        return "T";
    case TypeKind::Sum:
        if (const auto *sum = this->sum(resolved); sum != nullptr) {
            std::string result;
            for (size_t index = 0; index < sum->members.size(); ++index) {
                if (index != 0)
                    result += " | ";
                result += typeToString(sum->members[index]);
            }
            return result;
        }
        return "sum";
    case TypeKind::Failable:
        if (const auto *fail = failable(resolved); fail != nullptr)
            return "!" + typeToString(fail->inner);
        return "failable";
    case TypeKind::Pack:
        return "pack";
    case TypeKind::Dyn:
        if (const auto *dyn = this->dyn_type(resolved); dyn != nullptr)
            return "dyn " + typeToString(dyn->target);
        return "dyn";
    case TypeKind::Opaque:
        return "opaque";
    case TypeKind::Alias:
        if (const auto *alias = this->alias(resolved); alias != nullptr)
            return typeToString(alias->target);
        return "alias";
    case TypeKind::Nominal:
        if (const auto *nom = nominal(resolved); nom != nullptr)
            return std::string(nom->name);
        return "nominal";
    case TypeKind::Incomplete:
        return "incomplete";
    case TypeKind::Qualified:
        if (const auto *qual = qualified(resolved); qual != nullptr)
            return typeToString(qual->inner);
        return "qualified";
    }
    if (entry->kind == EntryKind::Name && !entry->name_view.empty())
        return std::string(entry->name_view);
    return "unknown";
}

void TypeTable::genericParamOrigin(TypeId id, uint32_t *decl_id,
                                   uint32_t *param_index) const noexcept {
    const auto *entry = findEntry(id);
    if (decl_id != nullptr)
        *decl_id = entry != nullptr && entry->kind == EntryKind::GenericParam
                       ? entry->generic_decl_id
                       : 0U;
    if (param_index != nullptr)
        *param_index = entry != nullptr && entry->kind == EntryKind::GenericParam
                           ? entry->generic_param_index
                           : 0U;
}

const PointerType *TypeTable::pointer(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Pointer ? &entry->pointer_ty : nullptr;
}

const OptionalType *TypeTable::optional(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Optional ? &entry->optional_ty : nullptr;
}

const ArrayType *TypeTable::array(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Array ? &entry->array_ty : nullptr;
}

const IntegerType *TypeTable::integer(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Integer ? &entry->integer : nullptr;
}

const FloatType *TypeTable::float_kind(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Float ? &entry->float_ty : nullptr;
}

const FunctionType *TypeTable::function(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    if (entry == nullptr)
        return nullptr;
    if (entry->kind == EntryKind::Function || entry->kind == EntryKind::StateFunction)
        return entry->fn;
    return nullptr;
}

const StructType *TypeTable::struct_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Struct ? entry->struct_ty : nullptr;
}

const EnumType *TypeTable::enum_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Enum ? entry->enum_ty : nullptr;
}

const UnionType *TypeTable::union_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Union ? entry->union_ty : nullptr;
}

const TraitType *TypeTable::trait(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Trait ? entry->trait_ty : nullptr;
}

const TypeVarType *TypeTable::type_var(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::TypeVar ? entry->var_ty : nullptr;
}

const SumType *TypeTable::sum(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Sum ? entry->sum_ty : nullptr;
}

const SliceType *TypeTable::slice(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Slice ? &entry->slice_ty : nullptr;
}

const FailableType *TypeTable::failable(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Failable ? &entry->failable_ty : nullptr;
}

const PackType *TypeTable::pack(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Pack ? entry->pack_ty : nullptr;
}

const DynType *TypeTable::dyn_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Dyn ? entry->dyn_ty : nullptr;
}

const OpaqueType *TypeTable::opaque_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Opaque ? &entry->opaque_ty : nullptr;
}

const AliasType *TypeTable::alias(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Alias ? entry->alias_ty : nullptr;
}

const NominalType *TypeTable::nominal(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Nominal ? entry->nominal_ty : nullptr;
}

const QualifiedType *TypeTable::qualified(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Qualified ? entry->qualified_ty : nullptr;
}

TypeId TypeTable::stripQualifiers(TypeId id) const noexcept {
    for (unsigned guard = 0; guard < 8U; ++guard) {
        const auto *qual = qualified(canonical(id));
        if (qual == nullptr)
            break;
        id = qual->inner;
    }
    return canonical(id);
}

const IncompleteType *TypeTable::incomplete(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Incomplete ? entry->incomplete_ty : nullptr;
}

size_t TypeTable::size() const noexcept {
    return entries_.size();
}

void TypeTable::ConformanceTable::registerConformance(TypeId type, TypeId trait) {
    const std::string rendered_type =
        table_ != nullptr ? table_->typeToString(type) : std::string{};
    const std::string rendered_trait =
        table_ != nullptr ? table_->typeToString(trait) : std::string{};
    for (const auto &existing : conformances_) {
        if (existing.type == rendered_type && existing.trait == rendered_trait)
            return;
    }
    conformances_.push(Conformance{std::move(rendered_type), std::move(rendered_trait)});
}

bool TypeTable::ConformanceTable::satisfies(TypeId type, TypeId trait_or_interface) const {
    if (!type || !trait_or_interface)
        return false;
    for (const auto &existing : conformances_) {
        const std::string rendered_type =
            table_ != nullptr ? table_->typeToString(type) : std::string{};
        const std::string rendered_trait =
            table_ != nullptr ? table_->typeToString(trait_or_interface) : std::string{};
        if (existing.type == rendered_type && existing.trait == rendered_trait)
            return true;
    }
    // Structural interface satisfaction is implemented by PerModuleSema,
    // which has access to declared field names and types. The shared table only
    // stores nominal conformance edges; sema consults this query before falling
    // back to its own interface check.
    return false;
}

TypeId TypeTable::lookupNamed(std::string_view name) const noexcept {
    const auto *value = named_registry_.get(name);
    return value ? *value : kInvalidTypeId;
}

std::string_view TypeTable::namedTypeName(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry != nullptr ? entry->name_view : std::string_view{};
}

TypeId TypeTable::canonical(TypeId id) const noexcept {
    for (unsigned guard = 0; guard < 8U; ++guard) {
        const auto *entry = findEntry(id);
        if (entry == nullptr || entry->kind != EntryKind::Name || entry->name_view.empty())
            break;
        const auto *registered = named_registry_.get(entry->name_view);
        if (registered == nullptr || *registered == id)
            break;
        id = *registered;
    }
    return id;
}

TypeId TypeTable::findOrCreateNamed(std::string_view name, TypeKind kind) {
    const auto *existing = named_registry_.get(name);
    if (existing)
        return *existing;
    auto id = internName(name, kind);
    registerNamed(name, id);
    return id;
}

void TypeTable::registerNamed(std::string_view name, TypeId id) {
    named_registry_.insert(persistString(name), id);
}

std::string_view TypeTable::persistString(std::string_view name) {
    char *buffer = static_cast<char *>(arena_->alloc(name.size(), 1));
    std::memcpy(buffer, name.data(), name.size());
    return std::string_view(buffer, name.size());
}

types::OwnershipKind mapOwnership(frontend::OwnershipKind kind) noexcept {
    switch (kind) {
    case frontend::OwnershipKind::Unique:
        return types::OwnershipKind::Unique;
    case frontend::OwnershipKind::Share:
        return types::OwnershipKind::Share;
    case frontend::OwnershipKind::Lend:
        return types::OwnershipKind::Lend;
    case frontend::OwnershipKind::View:
        return types::OwnershipKind::View;
    case frontend::OwnershipKind::Belong:
        return types::OwnershipKind::Belong;
    case frontend::OwnershipKind::Default:
        break;
    }
    return types::OwnershipKind::Default;
}

TypeId TypeTable::lowerTypeExpr(const frontend::FrontendSnapshot &snapshot,
                                frontend::TypeExprId id) noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto &type_expressions = snapshot.typeExpressions();
    if (id.value > type_expressions.size())
        return kInvalidTypeId;
    const auto &type = type_expressions[id.value - 1U];
    // A memory qualifier wraps the type it annotates; `resolve`/`stripQualifiers`
    // look through the wrapper so unification is unaffected.
    if (type.ownership != frontend::OwnershipKind::Default || type.isMut) {
        frontend::TypeExpression bare = type;
        bare.ownership                = frontend::OwnershipKind::Default;
        bare.isMut                    = false;
        bare.hasMutKeyword            = false;
        const TypeId inner            = lowerTypeExprBare(snapshot, bare);
        if (!inner)
            return kInvalidTypeId;
        return internQualified(inner, mapOwnership(type.ownership), type.isMut);
    }
    return lowerTypeExprBare(snapshot, type);
}

TypeId TypeTable::lowerTypeExprBare(const frontend::FrontendSnapshot &snapshot,
                                    const frontend::TypeExpression &type) noexcept {
    switch (type.kind) {
    case frontend::TypeExprKind::Name: {
        // Do not invent a permissive Unknown for unresolved names; the caller (PerModuleSema)
        // will diagnose them. If the name has been registered (e.g. from another module) just
        // forward it, otherwise return the Error sentinel.
        const auto *found = named_registry_.get(type.name);
        return found ? *found : kInvalidTypeId;
    }
    case frontend::TypeExprKind::Pointer:
        if (!type.arguments.empty())
            return internPointer(lowerTypeExpr(snapshot, type.arguments[0]));
        return internPointer(kInvalidTypeId);
    case frontend::TypeExprKind::Optional:
        if (!type.arguments.empty())
            return internOptional(lowerTypeExpr(snapshot, type.arguments[0]));
        return internOptional(kInvalidTypeId);
    case frontend::TypeExprKind::Array:
        if (!type.arguments.empty())
            return internArray(lowerTypeExpr(snapshot, type.arguments[0]), type.arrayLength);
        return internArray(kInvalidTypeId, type.arrayLength);
    case frontend::TypeExprKind::Function: {
        auto &params = makeStorage();
        for (size_t i = 0; i + 1 < type.arguments.size(); ++i)
            params.push(lowerTypeExpr(snapshot, type.arguments[i]));
        TypeId result = type.arguments.empty() ? kInvalidTypeId
                                               : lowerTypeExpr(snapshot, type.arguments.back());
        return type.isStateFunctionType ? internStateFunction(params, result)
                                        : internFunction(params, result);
    }
    case frontend::TypeExprKind::Slice:
        if (!type.arguments.empty())
            return internSlice(lowerTypeExpr(snapshot, type.arguments[0]));
        return internSlice(kInvalidTypeId);
    case frontend::TypeExprKind::Opaque:
        // `raw opaque` is pointer-to-void. `void` is registered by PerModuleSema before any
        // type expression is lowered, so the lookup only fails on a table with no primitives.
        return internPointer(lookupNamed("void"));
    case frontend::TypeExprKind::OpaqueTagged:
        return internOpaque();
    case frontend::TypeExprKind::Parenthesized:
        if (type.arguments.empty())
            return kInvalidTypeId;
        return lowerTypeExpr(snapshot, type.arguments[0]);
    case frontend::TypeExprKind::Pack: {
        auto &members = makeTypeStorage();
        auto &names   = makeStringStorage();
        for (const auto &arg : type.arguments)
            members.push(lowerTypeExpr(snapshot, arg));
        for (const auto &name : type.member_names)
            names.push(name);
        if (names.size() != members.size())
            return kInvalidTypeId;
        return internPack(members, names);
    }
    case frontend::TypeExprKind::Dyn:
        if (type.arguments.empty())
            return kInvalidTypeId;
        return internDyn(lowerTypeExpr(snapshot, type.arguments[0]));
    case frontend::TypeExprKind::Error:
        return kInvalidTypeId;
    }
    return kInvalidTypeId;
}

const TypeTable::Entry *TypeTable::findEntry(TypeId id) const noexcept {
    if (id.intern_seq == 0 || id.intern_seq > entries_.size())
        return nullptr;
    return &entries_[id.intern_seq - 1U];
}

} // namespace zith::sema::modern
