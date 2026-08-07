#pragma once

#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "types/type-kind.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace zith::sema::modern {

struct TypeId {
    uint32_t intern_seq = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return intern_seq != 0;
    }
    friend constexpr bool operator==(TypeId, TypeId) = default;
};

inline constexpr TypeId kInvalidTypeId{};

enum class TypeKind : uint8_t {
    Error,
    /// Compiler-internal type for a binding whose type is not yet known.
    /// Not available as a user-written type name.
    Invalid,
    Void,
    Never,
    Bool,
    Char,
    Integer,
    Float,
    String,
    Pointer,
    Optional,
    Array,
    Function,
    Struct,
    Enum,
    Union,
    Trait,
    TypeVar,
    Unknown,
    Incomplete,
    Sum,
    Slice,
    Failable,
    Pack,
    Alias,
    /// A `type Name = T` declaration: identity is nominal, but layout comes
    /// from the single underlying field.
    Nominal,
    GenericParam,
    /// A type carrying a memory-model qualifier (`lend T`, `view T`, ...).
    /// `resolve()` looks through it, so unification is unaffected.
    Qualified
};

struct IntegerType {
    uint8_t bits  = 0;
    bool isSigned = true;
};
struct FloatType {
    uint8_t bits = 0;
};
struct PointerType {
    TypeId pointee;
};
struct OptionalType {
    TypeId inner;
};
struct ArrayType {
    TypeId element;
    uint64_t size = 0;
};
struct FunctionType {
    memory::DynArray<TypeId> &params;
    TypeId result;
};
struct StructType {
    std::string_view name;
    memory::DynArray<TypeId> &fields;
    memory::DynArray<std::string_view> &field_names;
};
struct EnumType {
    std::string_view name;
    TypeId underlying;
    memory::DynArray<std::string_view> &variant_names;
    memory::DynArray<int64_t> &discriminants;
};
struct UnionType {
    std::string_view name;
    memory::DynArray<TypeId> &members;
};
struct TraitType {
    std::string_view name;
};
struct TypeVarType {
    uint32_t id;
};
struct SumType {
    memory::DynArray<TypeId> &members;
};
struct SliceType {
    TypeId element;
};
struct FailableType {
    TypeId inner;
};
struct PackType {
    memory::DynArray<TypeId> &members;
    memory::DynArray<std::string_view> &names;
};
struct AliasType {
    TypeId target;
};
struct NominalType {
    std::string_view name;
    TypeId target;
    memory::DynArray<TypeId> &fields;
};
struct IncompleteType {
    TypeId base;
    memory::DynArray<TypeId> &args;
};
struct QualifiedType {
    TypeId inner;
    types::OwnershipKind ownership = types::OwnershipKind::Default;
    bool isMut                     = false;
};

/// Maps a parsed ownership prefix onto the shared `types::OwnershipKind` enum.
[[nodiscard]] types::OwnershipKind mapOwnership(frontend::OwnershipKind kind) noexcept;

class TypeTable {
public:
    explicit TypeTable(memory::Arena &arena);

    [[nodiscard]] TypeId internName(std::string_view name, TypeKind kind = TypeKind::Unknown);
    /// The internal "not inferred yet" type used for uninitialized local bindings.
    [[nodiscard]] TypeId internInvalid();
    [[nodiscard]] TypeId internInteger(IntegerType int_type);
    [[nodiscard]] TypeId internFloat(FloatType float_type);
    [[nodiscard]] TypeId internPointer(TypeId pointee);
    [[nodiscard]] TypeId internOptional(TypeId inner);
    [[nodiscard]] TypeId internArray(TypeId element, uint64_t size);
    [[nodiscard]] TypeId internFunction(memory::DynArray<TypeId> &params, TypeId result);
    [[nodiscard]] TypeId internStruct(std::string_view name, memory::DynArray<TypeId> &fields,
                                      memory::DynArray<std::string_view> *field_names = nullptr);
    [[nodiscard]] int fieldIndex(TypeId struct_type, std::string_view name) const noexcept;
    [[nodiscard]] TypeId internEnum(std::string_view name, TypeId underlying,
                                    memory::DynArray<std::string_view> &variant_names,
                                    memory::DynArray<int64_t> &discriminants);
    [[nodiscard]] TypeId internUnion(std::string_view name, memory::DynArray<TypeId> &members);
    [[nodiscard]] TypeId internTrait(std::string_view name);
    [[nodiscard]] TypeId internTypeVar();
    [[nodiscard]] TypeId internUnknown();
    [[nodiscard]] TypeId internGenericParam(uint32_t decl_id, uint32_t param_index);
    /// For TypeKind::GenericParam types: returns the declaration id and parameter
    /// index the type was interned for (via out-params).
    void genericParamOrigin(TypeId id, uint32_t *decl_id, uint32_t *param_index) const noexcept;
    [[nodiscard]] TypeId internIncomplete(TypeId base, memory::DynArray<TypeId> &args);
    [[nodiscard]] TypeId internSum(memory::DynArray<TypeId> &members);
    [[nodiscard]] TypeId internSlice(TypeId element);
    [[nodiscard]] TypeId internFailable(TypeId inner);
    [[nodiscard]] TypeId internPack(memory::DynArray<TypeId> &members,
                                    memory::DynArray<std::string_view> &names);
    [[nodiscard]] TypeId internAlias(TypeId target);
    [[nodiscard]] TypeId internNominal(std::string_view name, TypeId target);
    [[nodiscard]] TypeId internQualified(TypeId inner, types::OwnershipKind ownership, bool is_mut);

    [[nodiscard]] TypeKind kindOf(TypeId id) const noexcept;

    /// Stable diagnostic description of a type: `i32`, `*T`, `?T`, `[]T`,
    /// `[N]T`, named structs/enums, and function types.
    [[nodiscard]] std::string typeToString(TypeId id) const;

    [[nodiscard]] const PointerType *pointer(TypeId id) const noexcept;
    [[nodiscard]] const OptionalType *optional(TypeId id) const noexcept;
    [[nodiscard]] const ArrayType *array(TypeId id) const noexcept;
    [[nodiscard]] const IntegerType *integer(TypeId id) const noexcept;
    [[nodiscard]] const FloatType *float_kind(TypeId id) const noexcept;
    [[nodiscard]] const FunctionType *function(TypeId id) const noexcept;
    [[nodiscard]] const StructType *struct_type(TypeId id) const noexcept;
    [[nodiscard]] const EnumType *enum_type(TypeId id) const noexcept;
    [[nodiscard]] const UnionType *union_type(TypeId id) const noexcept;
    [[nodiscard]] const TraitType *trait(TypeId id) const noexcept;
    [[nodiscard]] const TypeVarType *type_var(TypeId id) const noexcept;
    [[nodiscard]] const SumType *sum(TypeId id) const noexcept;
    [[nodiscard]] const SliceType *slice(TypeId id) const noexcept;
    [[nodiscard]] const FailableType *failable(TypeId id) const noexcept;
    [[nodiscard]] const PackType *pack(TypeId id) const noexcept;
    [[nodiscard]] const AliasType *alias(TypeId id) const noexcept;
    [[nodiscard]] const NominalType *nominal(TypeId id) const noexcept;
    [[nodiscard]] const QualifiedType *qualified(TypeId id) const noexcept;
    /// Strips every qualifier and transparent alias/nominal layer from `id`.
    [[nodiscard]] TypeId stripQualifiers(TypeId id) const noexcept;
    [[nodiscard]] const IncompleteType *incomplete(TypeId id) const noexcept;

    [[nodiscard]] size_t size() const noexcept;

    /// Maps a nominal placeholder (created before the declaration was seen) onto the
    /// completed type registered under the same name. Idempotent for every other type.
    [[nodiscard]] TypeId canonical(TypeId id) const noexcept;
    [[nodiscard]] TypeId lookupNamed(std::string_view name) const noexcept;
    [[nodiscard]] TypeId findOrCreateNamed(std::string_view name, TypeKind kind);
    void registerNamed(std::string_view name, TypeId id);

    [[nodiscard]] TypeId lowerTypeExpr(const frontend::FrontendSnapshot &snapshot,
                                       frontend::TypeExprId id) noexcept;

    // Public helpers used by sema-modern to build composite types.
    [[nodiscard]] memory::DynArray<TypeId> &makeTypeStorage();
    [[nodiscard]] memory::DynArray<std::string_view> &makeStringStorage();
    [[nodiscard]] memory::DynArray<int64_t> &makeDiscStorage();

private:
    enum class EntryKind : uint8_t {
        Name,
        Invalid,
        Integer,
        Float,
        Pointer,
        Optional,
        Array,
        Function,
        Struct,
        Enum,
        Union,
        Trait,
        TypeVar,
        Unknown,
        Incomplete,
        Sum,
        Slice,
        Failable,
        Pack,
        Alias,
        Nominal,
        GenericParam,
        Qualified
    };

    struct Entry {
        TypeId id;
        EntryKind kind;
        TypeKind reported_kind;
        std::string_view name_view;
        IntegerType integer{};
        FloatType float_ty{};
        PointerType pointer_ty{};
        OptionalType optional_ty{};
        ArrayType array_ty{};
        FunctionType *fn              = nullptr;
        StructType *struct_ty         = nullptr;
        EnumType *enum_ty             = nullptr;
        UnionType *union_ty           = nullptr;
        TraitType *trait_ty           = nullptr;
        TypeVarType *var_ty           = nullptr;
        IncompleteType *incomplete_ty = nullptr;
        SumType *sum_ty               = nullptr;
        SliceType slice_ty{};
        FailableType failable_ty{};
        PackType *pack_ty                                = nullptr;
        AliasType *alias_ty                              = nullptr;
        NominalType *nominal_ty                          = nullptr;
        QualifiedType *qualified_ty                      = nullptr;
        uint32_t generic_decl_id                         = 0;
        uint32_t generic_param_index                     = 0;
        memory::DynArray<TypeId> *storage                = nullptr;
        memory::DynArray<TypeId> *storage2               = nullptr;
        memory::DynArray<std::string_view> *name_storage = nullptr;
        memory::DynArray<int64_t> *disc_storage          = nullptr;
        TypeId underlying                                = kInvalidTypeId;
    };

    memory::DynArray<Entry> entries_;
    memory::Arena *arena_;
    uint32_t next_seq_ = 1;
    uint32_t next_var_ = 1;
    memory::FlatMap<std::string_view, TypeId> named_registry_;

    /// Lowers `type` ignoring its own memory qualifier (the caller wraps the result).
    [[nodiscard]] TypeId lowerTypeExprBare(const frontend::FrontendSnapshot &snapshot,
                                           const frontend::TypeExpression &type) noexcept;

    Entry &pushEntry(EntryKind kind);
    memory::DynArray<TypeId> &makeStorage();
    memory::DynArray<std::string_view> &makeNameStorage();
    const Entry *findEntry(TypeId id) const noexcept;
};

} // namespace zith::sema::modern
