#include "sema/modern-types.hpp"

#include <stdexcept>

namespace zith::sema::modern {

TypeTable::TypeTable(memory::Arena &arena)
    : entries_(arena), arena_(&arena) {}

TypeTable::Entry &TypeTable::pushEntry(EntryKind kind) {
    entries_.push(Entry{TypeId{next_seq_++}, kind, TypeKind::Error, {}, {}, {}, {}, {}, {}, nullptr, nullptr});
    return entries_.back();
}

memory::DynArray<TypeId> &TypeTable::makeStorage() {
    return *arena_->make<memory::DynArray<TypeId>>(*arena_);
}

// --- Intern helpers ---

TypeId TypeTable::internName(std::string_view name, TypeKind kind) {
    auto &entry        = pushEntry(EntryKind::Name);
    entry.name_view    = name;
    entry.reported_kind = kind;
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

TypeId TypeTable::internStruct(std::string_view name, memory::DynArray<TypeId> &fields) {
    auto &entry         = pushEntry(EntryKind::Struct);
    entry.reported_kind = TypeKind::Struct;
    auto &storage       = makeStorage();
    for (auto &f : fields)
        storage.push(f);
    entry.struct_ty = arena_->make<StructType>(StructType{name, storage});
    entry.storage   = &storage;
    return entry.id;
}

// --- Queries ---

TypeKind TypeTable::kindOf(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry ? entry->reported_kind : TypeKind::Error;
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
    return entry && entry->kind == EntryKind::Function ? entry->fn : nullptr;
}

const StructType *TypeTable::struct_type(TypeId id) const noexcept {
    const auto *entry = findEntry(id);
    return entry && entry->kind == EntryKind::Struct ? entry->struct_ty : nullptr;
}

size_t TypeTable::size() const noexcept {
    return entries_.size();
}

const TypeTable::Entry *TypeTable::findEntry(TypeId id) const noexcept {
    if (id.intern_seq == 0 || id.intern_seq > entries_.size())
        return nullptr;
    return &entries_[id.intern_seq - 1U];
}

} // namespace zith::sema::modern
