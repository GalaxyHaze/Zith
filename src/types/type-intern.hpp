#pragma once

#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "types/type-id.hpp"
#include "types/type-kind.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zith::types {

struct StructField {
    memory::InternedId name;
    TypeId type;
};

struct StructDef {
    memory::InternedId name;
    memory::DynArray<StructField> fields;
};

struct EnumVariantDef {
    memory::InternedId name;
    int64_t discriminant;
};

struct EnumDef {
    memory::InternedId name;
    TypeId underlying;
    memory::DynArray<EnumVariantDef> variants;
};

struct UnionDef {
    memory::InternedId name;
    bool is_tagged;
    memory::DynArray<TypeId> members;
};

struct CanonicalTagMapping {
    TypeCanonicalId id;
    uint32_t tag = 0;
};

class TypeIntern {
    memory::Arena &arena_;
    memory::StringInterner &interner_;
    memory::DynArray<TypeData> types_;
    memory::DynArray<size_t> hashes_;
    memory::DynArray<StructDef> struct_defs_;
    memory::DynArray<EnumDef> enum_defs_;
    memory::DynArray<UnionDef> union_defs_;
    memory::FlatMap<memory::InternedId, TypeId> named_types_;
    std::string current_module_;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> canonical_tags_;

    size_t computeHash(const TypeData &data);

public:
    explicit TypeIntern(memory::Arena &arena, memory::StringInterner &interner);

    // ── Intern existing TypeData ─────────────────────────────────
    TypeId intern(TypeData data);

    // ── Convenience intern helpers ───────────────────────────────
    TypeId internInt(IntWidth w);
    TypeId internFloat(FloatWidth w);
    TypeId internPtr(TypeId pointee, bool is_mut = false,
                     OwnershipKind ownership = OwnershipKind::Default);
    TypeId internArray(TypeId elem, uint32_t count);
    TypeId internFn(memory::DynArray<TypeId> &params, TypeId ret);
    TypeId internFn(std::span<const TypeId> params, TypeId ret);
    TypeId internOptional(TypeId inner);
    TypeId internFailable(TypeId inner);
    TypeId internOpaqueTagged();
    TypeId internAlias(TypeId target);
    TypeId internNominal(std::string_view name, TypeId target);
    TypeId internTrait(std::string_view name);
    TypeId internQualified(TypeId inner, OwnershipKind ownership, bool is_mut = false);
    TypeId internTypeVar();
    TypeId internUnknown();
    TypeId registerNamedType(std::string_view name, TypeKind kind);
    void registerTypeAlias(std::string_view name, TypeId target);
    TypeId lookupNamedType(std::string_view name) const;
    TypeId internSlice(TypeId elem);
    TypeId internEnum(TypeId def_id);
    TypeId internUnion(TypeId def_id);
    TypeId internPack(memory::DynArray<TypeId> &members,
                      memory::DynArray<memory::InternedId> &names);
    TypeId internPack(std::span<const TypeId> members, std::span<const std::string_view> names);
    TypeId internSum(std::span<const TypeId> members);
    TypeId internSum(memory::DynArray<TypeId> &members);
    TypeId internDyn(TypeId target, size_t method_count = 0);
    TypeId internGenericParam(uint32_t decl_id, uint32_t param_index);
    TypeId internIncomplete(TypeId base, memory::DynArray<TypeId> &args);
    TypeId internIncomplete(TypeId base, std::span<const TypeId> args);

    /// Canonical id lookup plus persistent tag for opaque runtime use.  The
    /// caller normally calls this once per HIR opaque node; `tag` is the
    /// project-local value from the cache registry when one already exists,
    /// otherwise the in-memory registry assigns a stable tag for this session.
    uint32_t canonicalTag(TypeCanonicalId id, uint32_t tag);
    void setCanonicalTag(TypeCanonicalId id, uint32_t tag);
    [[nodiscard]] std::vector<CanonicalTagMapping> canonicalTagSnapshot() const;

    // ── Struct definition helpers ─────────────────────────────────
    TypeId defineStruct(std::string_view name);
    void addField(TypeId struct_type, std::string_view field_name, TypeId field_type);

    const StructDef &getStructDef(TypeId struct_type) const;
    StructDef &getStructDef(TypeId struct_type);
    const StructDef *lookupStructDef(uint32_t def_id) const noexcept;
    [[nodiscard]] size_t structDefCount() const noexcept {
        return struct_defs_.size();
    }
    size_t fieldCount(TypeId struct_type) const;
    const StructField &getField(TypeId struct_type, size_t index) const;
    bool hasField(TypeId struct_type, std::string_view name);
    TypeId fieldType(TypeId struct_type, std::string_view name);
    size_t fieldIndex(TypeId struct_type, std::string_view name) const;
    /// Pack members have the same aggregate shape as a struct for HIR and
    /// codegen; the helpers below accept both `TypeStruct` and `TypePack`.
    size_t memberCount(TypeId type) const;
    StructField memberAt(TypeId type, size_t index) const;
    int memberIndex(TypeId type, std::string_view name) const;

    TypeId defineEnum(std::string_view name, TypeId underlying);
    void setEnumUnderlying(TypeId enum_type, TypeId underlying);
    void addEnumVariant(TypeId enum_type, std::string_view name, int64_t discriminant);
    const EnumDef &getEnumDef(TypeId enum_type) const;
    EnumDef &getEnumDef(TypeId enum_type);
    const EnumDef *lookupEnumDef(uint32_t def_id) const noexcept;
    [[nodiscard]] size_t enumDefCount() const noexcept {
        return enum_defs_.size();
    }
    bool enumValue(TypeId enum_type, std::string_view name, int64_t &value) const;

    TypeId defineUnion(std::string_view name, bool is_tagged);
    void addUnionMember(TypeId union_type, TypeId member);
    const UnionDef &getUnionDef(TypeId union_type) const;
    UnionDef &getUnionDef(TypeId union_type);
    const UnionDef *lookupUnionDef(uint32_t def_id) const noexcept;
    [[nodiscard]] size_t unionDefCount() const noexcept {
        return union_defs_.size();
    }

    // ── Query ────────────────────────────────────────────────────
    void setCurrentModule(std::string_view module);
    [[nodiscard]] const std::string &currentModule() const noexcept {
        return current_module_;
    }
    const TypeData &lookup(TypeId id) const;
    TypeKind kindOf(TypeId id) const;
    size_t count() const noexcept;
    const memory::StringInterner &interner() const noexcept {
        return interner_;
    }
};

/// Safe default returned by `getUnionDef` when the requested type is not a
/// defined union. Callers should prefer `lookupUnionDef` when they already
/// hold a def id. The static arena keeps the member DynArray valid for the
/// whole process; this default contains no members.
inline const UnionDef &emptyUnionDef() {
    static memory::Arena *arena = new memory::Arena;
    static const UnionDef *def =
        new UnionDef{memory::InternedId{0}, false, memory::DynArray<TypeId>(*arena)};
    return *def;
}

} // namespace zith::types
