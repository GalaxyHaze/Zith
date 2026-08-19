#include "sema/type-table.hpp"

#include <cstring>
#include <string>

namespace toolkit::sema {
namespace {

bool copyName(common::memory::Arena &arena, std::string_view value,
              std::string_view &out) {
    if (value.empty()) {
        out = {};
        return true;
    }
    void *storage = arena.alloc(value.size(), 1);
    if (storage == nullptr)
        return false;
    std::memcpy(storage, value.data(), value.size());
    out = std::string_view{static_cast<const char *>(storage), value.size()};
    return true;
}

} // namespace

TypeTable::TypeTable(common::memory::Arena &arena,
                     common::memory::StringInterner &interner)
    : arena_(arena), typeContext_(arena, interner), entries_(arena), named_() {
    void_ = registerBuiltinFromContext("void", TypeKind::Void);
    bool_ = registerBuiltinFromContext("bool", TypeKind::Bool);
    char_ = registerBuiltinFromContext("char", TypeKind::Char);
    i32_ = registerBuiltinFromContext("i32", TypeKind::Integer);
    i64_ = registerBuiltinFromContext("i64", TypeKind::Integer);
    f32_ = registerBuiltinFromContext("f32", TypeKind::Float);
    f64_ = registerBuiltinFromContext("f64", TypeKind::Float);
    u64_ = registerBuiltinFromContext("u64", TypeKind::Integer);

    static constexpr std::string_view kIntegers[] = {
        "i8", "i16", "u8", "u16", "u32", "u64",
    };
    for (const auto name : kIntegers) {
        if (lookupNamed(name) == kInvalidTypeId)
            (void)registerBuiltinFromContext(name, TypeKind::Integer);
    }
    if (lookupNamed("string") == kInvalidTypeId)
        (void)registerBuiltinFromContext("string", TypeKind::String);
    error_ = push(TypeKind::Error, "error");
    invalid_ = push(TypeKind::Invalid, "invalid");
    null_ = push(TypeKind::Null, "null");
    registerNamed("error", error_);
    registerNamed("invalid", invalid_);
    registerNamed("null", null_);
}

TypeId TypeTable::push(TypeKind kind, std::string_view name) {
    const TypeId id = static_cast<TypeId>(entries_.size());
    entries_.push(TypeDesc{
        .id = id,
        .kind = kind,
        .name = name,
    });
    return id;
}

TypeId TypeTable::registerBuiltinFromContext(std::string_view name,
                                             TypeKind kind) {
    if (const type_system::TypeDesc *builtin = typeContext_.find(name);
        builtin != nullptr) {
        while (entries_.size() <= static_cast<std::size_t>(builtin->id))
            entries_.push(TypeDesc{});
        TypeDesc &desc = entries_[builtin->id];
        desc.id = builtin->id;
        desc.kind = kind;
        desc.name = builtin->name;
        desc.bits = builtin->bits;
        desc.isSigned = builtin->isSigned;
        registerNamed(name, builtin->id);
        return builtin->id;
    }
    const TypeId id = push(kind, name);
    registerNamed(name, id);
    return id;
}

TypeId TypeTable::internName(std::string_view name, TypeKind kind) {
    if (const TypeId existing = lookupNamed(name);
        existing != kInvalidTypeId)
        return existing;
    const TypeId id = push(kind, name);
    registerNamed(name, id);
    return id;
}

TypeId TypeTable::internPointer(TypeId pointee) {
    const TypeId id = push(TypeKind::Pointer, "*");
    entries_[id].inner = pointee;
    return id;
}

TypeId TypeTable::internOptional(TypeId inner) {
    const TypeId id = push(TypeKind::Optional, "?");
    entries_[id].inner = inner;
    return id;
}

TypeId TypeTable::internArray(TypeId element, std::uint64_t length) {
    const TypeId id = push(TypeKind::Array, "[]");
    entries_[id].inner = element;
    entries_[id].length = length;
    return id;
}

TypeId TypeTable::internSlice(TypeId element) {
    const TypeId id = push(TypeKind::Slice, "[]");
    entries_[id].inner = element;
    return id;
}

TypeId TypeTable::internFunction(common::memory::DynArray<TypeId> &params,
                                 TypeId result) {
    common::memory::DynArray<TypeId> &storage = makeTypeStorage();
    storage.appendRange(params.data(), params.size());
    const TypeId id = push(TypeKind::Function, "fn");
    entries_[id].components = &storage;
    entries_[id].inner = result;
    return id;
}

TypeId TypeTable::internStruct(
    std::string_view name, common::memory::DynArray<TypeId> &fields,
    common::memory::DynArray<std::string_view> &names) {
    const TypeId id = push(TypeKind::Struct, name);
    entries_[id].components = &fields;
    entries_[id].names = &names;
    return id;
}

TypeId TypeTable::internEnum(
    std::string_view name, TypeId underlying,
    common::memory::DynArray<std::string_view> &variantNames,
    common::memory::DynArray<std::int64_t> &discriminants) {
    const TypeId id = push(TypeKind::Enum, name);
    entries_[id].inner = underlying;
    entries_[id].names = &variantNames;
    entries_[id].discriminants = &discriminants;
    return id;
}

TypeId TypeTable::internUnion(
    std::string_view name, common::memory::DynArray<TypeId> &members) {
    const TypeId id = push(TypeKind::Union, name);
    entries_[id].components = &members;
    return id;
}

TypeId TypeTable::internTrait(std::string_view name) {
    return push(TypeKind::Trait, name);
}

TypeId TypeTable::internAlias(TypeId target) {
    const TypeId id = push(TypeKind::Alias, "alias");
    entries_[id].inner = target;
    return id;
}

TypeId TypeTable::internNominal(std::string_view name, TypeId target) {
    const TypeId id = push(TypeKind::Nominal, name);
    entries_[id].inner = target;
    return id;
}

TypeId TypeTable::internGeneric(std::string_view name) {
    return push(TypeKind::Generic, name);
}

TypeId TypeTable::internPlaceholder(std::string_view name) {
    if (const TypeId existing = lookupNamed(name);
        existing != kInvalidTypeId)
        return existing;
    const TypeId id = push(TypeKind::Placeholder, name);
    registerNamed(name, id);
    return id;
}

TypeId TypeTable::internQualified(TypeId inner, Ownership ownership,
                                  bool isMut) {
    const TypeId id = push(TypeKind::Qualified, "qualified");
    entries_[id].inner = inner;
    entries_[id].ownership = ownership;
    entries_[id].isMut = isMut;
    return id;
}

void TypeTable::registerNamed(std::string_view name, TypeId id) {
    if (name.empty())
        return;
    named_.insert(name, id);
}

TypeId TypeTable::lookupNamed(std::string_view name) const noexcept {
    const TypeId *id = named_.get(name);
    return id != nullptr ? *id : kInvalidTypeId;
}

TypeId TypeTable::findOrCreateNamed(std::string_view name, TypeKind kind) {
    if (const TypeId existing = lookupNamed(name);
        existing != kInvalidTypeId)
        return existing;
    return internName(name, kind);
}

const TypeDesc *TypeTable::find(TypeId id) const noexcept {
    return id < entries_.size() ? &entries_[id] : nullptr;
}

TypeKind TypeTable::kindOf(TypeId id) const noexcept {
    const TypeDesc *desc = find(resolve(id));
    return desc != nullptr ? desc->kind : TypeKind::Error;
}

bool TypeTable::isInteger(TypeId id) const noexcept {
    return kindOf(id) == TypeKind::Integer;
}

bool TypeTable::isFloat(TypeId id) const noexcept {
    return kindOf(id) == TypeKind::Float;
}

bool TypeTable::isNumeric(TypeId id) const noexcept {
    const TypeKind kind = kindOf(id);
    return kind == TypeKind::Integer || kind == TypeKind::Float ||
           kind == TypeKind::Char;
}

const TypeDesc *TypeTable::qualified(TypeId id) const noexcept {
    const TypeDesc *desc = find(id);
    for (int guard = 0; desc != nullptr && guard < 64; ++guard) {
        if (desc->kind == TypeKind::Qualified)
            return desc;
        desc = find(desc->inner);
    }
    return nullptr;
}

TypeId TypeTable::resolve(TypeId id) const noexcept {
    for (int guard = 0; guard < 64 && id != kInvalidTypeId; ++guard) {
        const TypeDesc *desc = find(id);
        if (desc == nullptr)
            return id;
        switch (desc->kind) {
        case TypeKind::Qualified:
        case TypeKind::Alias:
        case TypeKind::Nominal:
            id = desc->inner;
            break;
        case TypeKind::Placeholder: {
            const TypeId named = lookupNamed(desc->name);
            if (named != kInvalidTypeId && named != id)
                id = named;
            else
                return id;
            break;
        }
        default:
            return id;
        }
    }
    return id;
}

std::string TypeTable::toString(TypeId id) const {
    const TypeId concrete = resolve(id);
    const TypeDesc *desc = find(concrete);
    if (desc == nullptr || desc->name.empty())
        return "error";
    if (!desc->name.empty() && desc->name != "*" && desc->name != "?" &&
        desc->name != "[]" && desc->name != "fn" && desc->name != "alias" &&
        desc->name != "qualified")
        return std::string(desc->name);

    switch (desc->kind) {
    case TypeKind::Pointer:
        return "*" + toString(desc->inner);
    case TypeKind::Optional:
        return "?" + toString(desc->inner);
    case TypeKind::Array:
        return "[" + std::to_string(desc->length) + "]" +
               toString(desc->inner);
    case TypeKind::Slice:
        return "[]" + toString(desc->inner);
    case TypeKind::Function: {
        std::string out = "(";
        if (desc->components != nullptr) {
            for (std::size_t index = 0; index < desc->components->size();
                 ++index) {
                if (index != 0)
                    out += ", ";
                out += toString((*desc->components)[index]);
            }
        }
        out += "): ";
        out += toString(desc->inner);
        return out;
    }
    default:
        return std::string(desc->name);
    }
}

int TypeTable::fieldIndex(TypeId structType,
                          std::string_view name) const noexcept {
    const TypeId concrete = resolve(structType);
    const TypeDesc *desc = find(concrete);
    if (desc == nullptr || desc->names == nullptr)
        return -1;
    for (std::size_t index = 0; index < desc->names->size(); ++index) {
        if ((*desc->names)[index] == name)
            return static_cast<int>(index);
    }
    return -1;
}

TypeId TypeTable::lowerBareTypeExpr(const generated_ast::TypeExpr *type,
                                    bool &reported) {
    reported = false;
    if (type == nullptr)
        return error_;
    switch (static_cast<sample::TypeExprKind>(type->kind)) {
    case sample::TypeExprKind::Name: {
        const TypeId named = lookupNamed(type->name);
        if (named != kInvalidTypeId)
            return named;
        reported = true;
        return error_;
    }
    case sample::TypeExprKind::Pointer: {
        const TypeId inner = type->arguments.empty()
                                 ? error_
                                 : lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                                       type->arguments[0]));
        if (resolve(inner) == resolve(void_)) {
            reported = true;
            return error_;
        }
        return internPointer(inner);
    }
    case sample::TypeExprKind::Optional: {
        const TypeId inner = type->arguments.empty()
                                 ? error_
                                 : lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                                       type->arguments[0]));
        return internOptional(inner);
    }
    case sample::TypeExprKind::Array:
        return internArray(type->arguments.empty()
                               ? error_
                               : lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                                     type->arguments[0])),
                           type->arrayLength);
    case sample::TypeExprKind::Slice:
        return internSlice(type->arguments.empty()
                               ? error_
                               : lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                                     type->arguments[0])));
    case sample::TypeExprKind::Function: {
        common::memory::DynArray<TypeId> &params = makeTypeStorage();
        for (std::size_t index = 0; index + 1 < type->arguments.size(); ++index)
            params.push(lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                type->arguments[index])));
        const TypeId result = type->arguments.empty()
                                  ? error_
                                  : lowerTypeExpr(static_cast<generated_ast::TypeExpr *>(
                                        type->arguments.back()));
        return internFunction(params, result);
    }
    case sample::TypeExprKind::Opaque:
        return internPointer(void_);
    case sample::TypeExprKind::Error:
        reported = true;
        return error_;
    }
    reported = true;
    return error_;
}

TypeId TypeTable::lowerTypeExpr(const generated_ast::TypeExpr *type) {
    bool reported = false;
    const TypeId bare = lowerBareTypeExpr(type, reported);
    if (type == nullptr || bare == kInvalidTypeId || reported)
        return bare;
    Ownership ownership = Ownership::Default;
    switch (static_cast<Ownership>(type->ownership)) {
    case Ownership::Lend:
    case Ownership::Share:
    case Ownership::View:
    case Ownership::Unique:
    case Ownership::Belong:
    case Ownership::Default:
        ownership = static_cast<Ownership>(type->ownership);
        break;
    }
    if (ownership != Ownership::Default || type->isMut)
        return internQualified(bare, ownership, type->isMut);
    return bare;
}

common::memory::DynArray<TypeId> &TypeTable::makeTypeStorage() {
    auto *storage = arena_.make<common::memory::DynArray<TypeId>>(arena_);
    return *storage;
}

common::memory::DynArray<std::string_view> &TypeTable::makeNameStorage() {
    auto *storage =
        arena_.make<common::memory::DynArray<std::string_view>>(arena_);
    return *storage;
}

common::memory::DynArray<std::int64_t> &TypeTable::makeDiscStorage() {
    auto *storage =
        arena_.make<common::memory::DynArray<std::int64_t>>(arena_);
    return *storage;
}

} // namespace toolkit::sema
