#include "type-intern.hpp"
#include "common/overloaded.hpp"

#include <cstring>

namespace zith::types {

namespace {

TypeKind kindFromIndex(size_t idx) {
    static constexpr TypeKind map[] = {
        TypeKind::Error, // TypeError → 0
        TypeKind::Never,        // TypeNever      → 1
        TypeKind::Void,         // TypeVoid       → 2
        TypeKind::Bool,         // TypeBool       → 3
        TypeKind::Char,         // TypeChar       → 4
        TypeKind::Int,          // TypeInt        → 5
        TypeKind::Float,        // TypeFloat      → 6
        TypeKind::Ptr,          // TypePtr        → 7
        TypeKind::Array,        // TypeArray      → 8
        TypeKind::Struct,       // TypeStruct     → 9
        TypeKind::Fn,           // TypeFn         → 10
        TypeKind::TypeVar,      // TypeTypeVar    → 11
        TypeKind::Optional,     // TypeOptional   → 12
        TypeKind::Failable,     // TypeFailable   → 13
        TypeKind::Alias,        // TypeAlias      → 14
        TypeKind::Nominal,      // TypeNominal    → 15
        TypeKind::Trait,        // TypeTrait      → 16
        TypeKind::Opaque,       // TypeOpaque     → 17
        TypeKind::Unknown,      // TypeUnknown    → 18
        TypeKind::Qualified,    // TypeQualified  → 19
        TypeKind::Slice,        // TypeSlice      → 20
        TypeKind::Enum,         // TypeEnum       → 21
        TypeKind::Union,        // TypeUnion      → 22
        TypeKind::Pack,         // TypePack       → 23
        TypeKind::Sum,          // TypeSum        → 24
        TypeKind::GenericParam, // TypeGenericParam → 25
        TypeKind::Incomplete,   // TypeIncomplete → 26
    };
    return (idx < std::size(map)) ? map[idx] : TypeKind::Error;
}

} // anonymous namespace

namespace {

bool typeDataEqual(const TypeData &a, const TypeData &b) {
    if (a.index() != b.index())
        return false;
    return visitType(
        a, b,
        common::overloaded{
            [](const TypeInt &ai, const TypeInt &bi) { return ai.width == bi.width; },
            [](const TypeFloat &af, const TypeFloat &bf) { return af.width == bf.width; },
            [](const TypePtr &pa, const TypePtr &pb) {
                return pa.pointee == pb.pointee && pa.is_mut == pb.is_mut &&
                       pa.ownership == pb.ownership;
            },
            [](const TypeArray &aa, const TypeArray &bb) {
                return aa.elem == bb.elem && aa.count == bb.count;
            },
            [](const TypeStruct &sa, const TypeStruct &sb) { return sa.def_id == sb.def_id; },
            [](const TypeFn &fa, const TypeFn &fb) {
                if (fa.param_count != fb.param_count)
                    return false;
                if (fa.ret != fb.ret)
                    return false;
                for (size_t i = 0; i < fa.param_count; i++)
                    if (fa.params[i] != fb.params[i])
                        return false;
                return true;
            },
            [](const TypeTypeVar &va, const TypeTypeVar &vb) { return va.id == vb.id; },
            [](const TypeOptional &oa, const TypeOptional &ob) { return oa.inner == ob.inner; },
            [](const TypeFailable &fa, const TypeFailable &fb) { return fa.inner == fb.inner; },
            [](const TypeAlias &aa, const TypeAlias &ab) { return aa.target == ab.target; },
            [](const TypeNominal &na, const TypeNominal &nb) {
                return na.name == nb.name && na.target == nb.target;
            },
            [](const TypeTrait &ta, const TypeTrait &tb) { return ta.name == tb.name; },
            [](const TypeQualified &qa, const TypeQualified &qb) {
                return qa.inner == qb.inner && qa.ownership == qb.ownership &&
                       qa.isMut == qb.isMut;
            },
            [](const TypeSlice &sa, const TypeSlice &sb) { return sa.elem == sb.elem; },
            [](const TypeEnum &ea, const TypeEnum &eb) { return ea.def_id == eb.def_id; },
            [](const TypeUnion &ua, const TypeUnion &ub) { return ua.def_id == ub.def_id; },
            [](const TypePack &pa, const TypePack &pb) {
                if (pa.count != pb.count)
                    return false;
                for (size_t i = 0; i < pa.count; i++) {
                    if (pa.members[i] != pb.members[i])
                        return false;
                    if (pa.names[i] != pb.names[i])
                        return false;
                }
                return true;
            },
            [](const TypeSum &sa, const TypeSum &sb) {
                if (sa.count != sb.count)
                    return false;
                for (size_t i = 0; i < sa.count; i++)
                    if (sa.members[i] != sb.members[i])
                        return false;
                return true;
            },
            [](const TypeGenericParam &ga, const TypeGenericParam &gb) {
                return ga.decl_id == gb.decl_id && ga.param_index == gb.param_index;
            },
            [](const TypeIncomplete &ia, const TypeIncomplete &ib) {
                if (ia.base != ib.base || ia.arg_count != ib.arg_count)
                    return false;
                for (size_t i = 0; i < ia.arg_count; i++)
                    if (ia.args[i] != ib.args[i])
                        return false;
                return true;
            },
            // Error, Never, Void, Bool, Char, Opaque, Unknown, TypeVar, Enum, Union — no fields
            // beyond the type itself
            [](const auto &, const auto &) -> bool { return true; },
        });
}

} // anonymous namespace

TypeIntern::TypeIntern(memory::Arena &arena, memory::StringInterner &interner)
    : arena_(arena), interner_(interner), types_(arena), hashes_(arena), struct_defs_(arena),
      enum_defs_(arena), union_defs_(arena) {
    types_.push(TypeError{});
    hashes_.push(computeHash(types_.back()));
    types_.push(TypeNever{});
    hashes_.push(computeHash(types_.back()));
    types_.push(TypeVoid{});
    hashes_.push(computeHash(types_.back()));
    types_.push(TypeBool{});
    hashes_.push(computeHash(types_.back()));
    types_.push(TypeChar{});
    hashes_.push(computeHash(types_.back()));
}

namespace {

uint64_t hashCombine(uint64_t h, uint64_t v) {
    return (h ^ v) * 1099511628211ULL;
}

} // anonymous namespace

size_t TypeIntern::computeHash(const TypeData &data) {
    uint64_t h = 14695981039346656037ULL; // FNV offset basis
    h          = hashCombine(h, static_cast<size_t>(data.index()));

    visitType(data,
              common::overloaded{
                  [&](const TypeInt &t) { h = hashCombine(h, static_cast<size_t>(t.width)); },
                  [&](const TypeFloat &t) { h = hashCombine(h, static_cast<size_t>(t.width)); },
                  [&](const TypePtr &p) {
                      h = hashCombine(h, p.pointee);
                      h = hashCombine(h, static_cast<size_t>(p.is_mut));
                      h = hashCombine(h, static_cast<size_t>(p.ownership));
                  },
                  [&](const TypeArray &a) {
                      h = hashCombine(h, a.elem);
                      h = hashCombine(h, a.count);
                  },
                  [&](const TypeStruct &s) { h = hashCombine(h, s.def_id); },
                  [&](const TypeFn &f) {
                      h = hashCombine(h, f.ret);
                      h = hashCombine(h, f.param_count);
                      for (size_t i = 0; i < f.param_count; i++)
                          h = hashCombine(h, f.params[i]);
                  },
                  [&](const TypeTypeVar &v) { h = hashCombine(h, v.id); },
                  [&](const TypeOptional &o) { h = hashCombine(h, o.inner); },
                  [&](const TypeFailable &f) { h = hashCombine(h, f.inner); },
                  [&](const TypeAlias &a) { h = hashCombine(h, a.target); },
                  [&](const TypeNominal &n) {
                      h = hashCombine(h, n.name);
                      h = hashCombine(h, n.target);
                  },
                  [&](const TypeTrait &t) { h = hashCombine(h, t.name); },
                  [&](const TypeQualified &q) {
                      h = hashCombine(h, q.inner);
                      h = hashCombine(h, static_cast<size_t>(q.ownership));
                      h = hashCombine(h, static_cast<size_t>(q.isMut));
                  },
                  [&](const TypeSlice &s) { h = hashCombine(h, s.elem); },
                  [&](const TypeEnum &e) { h = hashCombine(h, e.def_id); },
                  [&](const TypeUnion &u) { h = hashCombine(h, u.def_id); },
                  [&](const TypePack &p) {
                      h = hashCombine(h, p.count);
                      for (size_t i = 0; i < p.count; i++) {
                          h         = hashCombine(h, p.members[i]);
                          auto name = interner_.lookup(p.names[i]);
                          for (auto c : name)
                              h = hashCombine(h, static_cast<size_t>(c));
                      }
                  },
                  [&](const TypeSum &s) {
                      h = hashCombine(h, s.count);
                      for (size_t i = 0; i < s.count; i++)
                          h = hashCombine(h, s.members[i]);
                  },
                  [&](const TypeGenericParam &g) {
                      h = hashCombine(h, g.decl_id);
                      h = hashCombine(h, g.param_index);
                  },
                  [&](const TypeIncomplete &i) {
                      h = hashCombine(h, i.base);
                      h = hashCombine(h, i.arg_count);
                      for (size_t j = 0; j < i.arg_count; j++)
                          h = hashCombine(h, i.args[j]);
                  },
                  // Error, Never, Void, Bool, Char, Opaque, Unknown — no fields
                  [](const auto &) {},
              });
    return static_cast<size_t>(h);
}

TypeId TypeIntern::intern(TypeData data) {
    // Skip dedup for type variables — each call creates a fresh variable
    if (!std::holds_alternative<TypeTypeVar>(data)) {
        size_t hash = computeHash(data);
        for (TypeId i = kFirstCustom; i < static_cast<TypeId>(types_.size()); i++) {
            if (hashes_[i] == hash && typeDataEqual(types_[i], data))
                return i;
        }
    }
    TypeId id = static_cast<TypeId>(types_.size());
    hashes_.push(computeHash(data));
    types_.push(std::move(data));
    return id;
}

TypeId TypeIntern::internInt(IntWidth w) {
    return intern(TypeInt{w});
}

TypeId TypeIntern::internFloat(FloatWidth w) {
    return intern(TypeFloat{w});
}

TypeId TypeIntern::internPtr(TypeId pointee, bool is_mut, OwnershipKind ownership) {
    return intern(TypePtr{pointee, is_mut, ownership});
}

TypeId TypeIntern::internArray(TypeId elem, uint32_t count) {
    return intern(TypeArray{elem, count});
}

TypeId TypeIntern::internFn(std::span<const TypeId> params, TypeId ret) {
    auto *param_copy =
        static_cast<TypeId *>(arena_.alloc(params.size() * sizeof(TypeId), alignof(TypeId)));
    std::memcpy(param_copy, params.data(), params.size() * sizeof(TypeId));
    return intern(TypeFn{param_copy, params.size(), ret});
}

TypeId TypeIntern::internOptional(TypeId inner) {
    return intern(TypeOptional{inner});
}

TypeId TypeIntern::internFailable(TypeId inner) {
    return intern(TypeFailable{inner});
}

TypeId TypeIntern::internAlias(TypeId target) {
    return intern(TypeAlias{target});
}

TypeId TypeIntern::internNominal(std::string_view name, TypeId target) {
    return intern(TypeNominal{interner_.intern(name), target});
}

TypeId TypeIntern::internTrait(std::string_view name) {
    return intern(TypeTrait{interner_.intern(name)});
}

TypeId TypeIntern::internQualified(TypeId inner, OwnershipKind ownership, bool is_mut) {
    return intern(TypeQualified{inner, ownership, is_mut});
}

TypeId TypeIntern::internTypeVar() {
    return intern(TypeTypeVar{static_cast<uint32_t>(types_.size())});
}

TypeId TypeIntern::internUnknown() {
    return intern(TypeUnknown{});
}

TypeId TypeIntern::registerNamedType(std::string_view name, TypeKind kind) {
    const auto id          = interner_.intern(name);
    const auto *existing   = named_types_.get(id);
    if (existing != nullptr)
        return *existing;

    TypeId tid = kErrorType;
    switch (kind) {
    case TypeKind::Struct:
        tid = defineStruct(name);
        break;
    case TypeKind::Enum:
        tid = defineEnum(name, kErrorType);
        break;
    case TypeKind::Union:
        tid = defineUnion(name, false);
        break;
    default:
        return kErrorType;
    }
    named_types_.insert(id, tid);
    return tid;
}

TypeId TypeIntern::lookupNamedType(std::string_view name) const {
    const auto *existing = named_types_.get(interner_.intern(name));
    return existing != nullptr ? *existing : kErrorType;
}

void TypeIntern::registerTypeAlias(std::string_view name, TypeId target) {
    named_types_.insert(interner_.intern(name), target);
}

TypeId TypeIntern::internSlice(TypeId elem) {
    return intern(TypeSlice{elem});
}

TypeId TypeIntern::internEnum(TypeId def_id) {
    return intern(TypeEnum{def_id});
}

TypeId TypeIntern::internUnion(TypeId def_id) {
    return intern(TypeUnion{def_id});
}

TypeId TypeIntern::internPack(std::span<const TypeId> members,
                              std::span<const std::string_view> names) {
    size_t count = members.size();
    auto *m      = static_cast<TypeId *>(arena_.alloc(count * sizeof(TypeId), alignof(TypeId)));
    std::memcpy(m, members.data(), count * sizeof(TypeId));

    auto *n = static_cast<memory::InternedId *>(
        arena_.alloc(count * sizeof(memory::InternedId), alignof(memory::InternedId)));
    for (size_t i = 0; i < count; i++)
        n[i] = interner_.intern(names[i]);

    return intern(TypePack{m, n, count});
}

TypeId TypeIntern::internSum(std::span<const TypeId> members) {
    size_t count = members.size();
    auto *m      = static_cast<TypeId *>(arena_.alloc(count * sizeof(TypeId), alignof(TypeId)));
    std::memcpy(m, members.data(), count * sizeof(TypeId));
    return intern(TypeSum{m, count});
}

TypeId TypeIntern::internGenericParam(uint32_t decl_id, uint32_t param_index) {
    return intern(TypeGenericParam{decl_id, param_index});
}

TypeId TypeIntern::internIncomplete(TypeId base, std::span<const TypeId> args) {
    size_t count = args.size();
    auto *a      = static_cast<TypeId *>(arena_.alloc(count * sizeof(TypeId), alignof(TypeId)));
    std::memcpy(a, args.data(), count * sizeof(TypeId));
    return intern(TypeIncomplete{base, a, count});
}

// ── Struct definition helpers ──────────────────────────────────────

TypeId TypeIntern::defineStruct(std::string_view name) {
    TypeId def_id = static_cast<TypeId>(struct_defs_.size());
    struct_defs_.push(StructDef{interner_.intern(name), memory::DynArray<StructField>(arena_)});
    auto type = intern(TypeStruct{def_id});
    named_types_.insert(interner_.intern(name), type);
    return type;
}

void TypeIntern::addField(TypeId struct_type, std::string_view field_name, TypeId field_type) {
    auto &def = getStructDef(struct_type);
    def.fields.push(StructField{interner_.intern(field_name), field_type});
}

const StructDef &TypeIntern::getStructDef(TypeId struct_type) const {
    auto *td = std::get_if<TypeStruct>(&types_[struct_type]);
    return struct_defs_[td->def_id];
}

StructDef &TypeIntern::getStructDef(TypeId struct_type) {
    return const_cast<StructDef &>(const_cast<const TypeIntern *>(this)->getStructDef(struct_type));
}

const StructDef *TypeIntern::lookupStructDef(uint32_t def_id) const noexcept {
    return def_id < struct_defs_.size() ? &struct_defs_[def_id] : nullptr;
}

size_t TypeIntern::fieldCount(TypeId struct_type) const {
    return getStructDef(struct_type).fields.size();
}

const StructField &TypeIntern::getField(TypeId struct_type, size_t index) const {
    return getStructDef(struct_type).fields[index];
}

bool TypeIntern::hasField(TypeId struct_type, std::string_view name) {
    auto &def = getStructDef(struct_type);
    auto id   = interner_.intern(name);
    for (auto &f : def.fields) {
        if (f.name == id)
            return true;
    }
    return false;
}

TypeId TypeIntern::fieldType(TypeId struct_type, std::string_view name) {
    auto &def = getStructDef(struct_type);
    auto id   = interner_.intern(name);
    for (auto &f : def.fields) {
        if (f.name == id)
            return f.type;
    }
    return kErrorType;
}

size_t TypeIntern::fieldIndex(TypeId struct_type, std::string_view name) const {
    const auto &def = getStructDef(struct_type);
    for (size_t i = 0; i < def.fields.size(); ++i)
        if (interner_.lookup(def.fields[i].name) == name)
            return i;
    return static_cast<size_t>(-1);
}

TypeId TypeIntern::defineEnum(std::string_view name, TypeId underlying) {
    TypeId def_id = static_cast<TypeId>(enum_defs_.size());
    enum_defs_.push(
        EnumDef{interner_.intern(name), underlying, memory::DynArray<EnumVariantDef>(arena_)});
    auto type = intern(TypeEnum{def_id});
    named_types_.insert(interner_.intern(name), type);
    return type;
}

void TypeIntern::addEnumVariant(TypeId enum_type, std::string_view name, int64_t discriminant) {
    auto *td = std::get_if<TypeEnum>(&types_[enum_type]);
    if (td)
        enum_defs_[td->def_id].variants.push(EnumVariantDef{interner_.intern(name), discriminant});
}

void TypeIntern::setEnumUnderlying(TypeId enum_type, TypeId underlying) {
    if (auto *td = std::get_if<TypeEnum>(&types_[enum_type]))
        enum_defs_[td->def_id].underlying = underlying;
}

const EnumDef &TypeIntern::getEnumDef(TypeId enum_type) const {
    auto *td = std::get_if<TypeEnum>(&types_[enum_type]);
    return enum_defs_[td->def_id];
}

EnumDef &TypeIntern::getEnumDef(TypeId enum_type) {
    return const_cast<EnumDef &>(const_cast<const TypeIntern *>(this)->getEnumDef(enum_type));
}

const EnumDef *TypeIntern::lookupEnumDef(uint32_t def_id) const noexcept {
    return def_id < enum_defs_.size() ? &enum_defs_[def_id] : nullptr;
}

bool TypeIntern::enumValue(TypeId enum_type, std::string_view name, int64_t &value) const {
    const auto &def = getEnumDef(enum_type);
    for (const auto &variant : def.variants) {
        if (interner_.lookup(variant.name) == name) {
            value = variant.discriminant;
            return true;
        }
    }
    return false;
}

TypeId TypeIntern::defineUnion(std::string_view name, bool is_raw) {
    TypeId def_id = static_cast<TypeId>(union_defs_.size());
    union_defs_.push(UnionDef{interner_.intern(name), is_raw, memory::DynArray<TypeId>(arena_)});
    auto type = intern(TypeUnion{def_id});
    named_types_.insert(interner_.intern(name), type);
    return type;
}

void TypeIntern::addUnionMember(TypeId union_type, TypeId member) {
    auto *td = std::get_if<TypeUnion>(&types_[union_type]);
    if (td)
        union_defs_[td->def_id].members.push(member);
}

const UnionDef &TypeIntern::getUnionDef(TypeId union_type) const {
    auto *td = std::get_if<TypeUnion>(&types_[union_type]);
    return union_defs_[td->def_id];
}

UnionDef &TypeIntern::getUnionDef(TypeId union_type) {
    return const_cast<UnionDef &>(const_cast<const TypeIntern *>(this)->getUnionDef(union_type));
}

const UnionDef *TypeIntern::lookupUnionDef(uint32_t def_id) const noexcept {
    return def_id < union_defs_.size() ? &union_defs_[def_id] : nullptr;
}

// ── Query ─────────────────────────────────────────────────────────

const TypeData &TypeIntern::lookup(TypeId id) const {
    return types_[id];
}

TypeKind TypeIntern::kindOf(TypeId id) const {
    if (id == kInvalidType || id >= types_.size())
        return TypeKind::Error;
    return kindFromIndex(types_[id].index());
}

size_t TypeIntern::count() const noexcept {
    return types_.size();
}

} // namespace zith::types
