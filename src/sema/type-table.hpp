#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/string-interner.hpp"
#include "common/type-system/type-system.hpp"
#include "frontend/ast/ast.hpp"
#include "frontend/parser/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace toolkit::sema {

using TypeId = std::uint32_t;
inline constexpr TypeId kInvalidTypeId = ~TypeId{0};

enum class Ownership : std::uint8_t {
    Default = 0,
    Lend = 1,
    Share = 2,
    View = 3,
    Unique = 4,
    Belong = 5,
};

enum class TypeKind : std::uint8_t {
    Error,
    Invalid,
    Void,
    Bool,
    Char,
    String,
    Integer,
    Float,
    Pointer,
    Optional,
    Array,
    Slice,
    Function,
    Struct,
    Enum,
    Union,
    Trait,
    Alias,
    Nominal,
    Opaque,
    Null,
    Generic,
    Placeholder,
    Qualified,
};

struct TypeDesc {
    TypeId id = kInvalidTypeId;
    TypeKind kind = TypeKind::Error;
    std::string_view name;
    std::uint16_t bits = 0;
    bool isSigned = false;
    Ownership ownership = Ownership::Default;
    bool isMut = false;
    TypeId inner = kInvalidTypeId;
    std::uint64_t length = 0;
    const common::memory::DynArray<TypeId> *components = nullptr;
    const common::memory::DynArray<std::string_view> *names = nullptr;
    const common::memory::DynArray<std::int64_t> *discriminants = nullptr;
};

class TypeTable {
public:
    TypeTable(common::memory::Arena &arena,
              common::memory::StringInterner &interner);

    [[nodiscard]] TypeId error() const noexcept { return error_; }
    [[nodiscard]] TypeId invalid() const noexcept { return invalid_; }
    [[nodiscard]] TypeId null() const noexcept { return null_; }
    [[nodiscard]] TypeId voidType() const noexcept { return void_; }
    [[nodiscard]] TypeId boolType() const noexcept { return bool_; }
    [[nodiscard]] TypeId charType() const noexcept { return char_; }
    [[nodiscard]] TypeId i32Type() const noexcept { return i32_; }
    [[nodiscard]] TypeId i64Type() const noexcept { return i64_; }
    [[nodiscard]] TypeId f32Type() const noexcept { return f32_; }
    [[nodiscard]] TypeId f64Type() const noexcept { return f64_; }
    [[nodiscard]] TypeId u64Type() const noexcept { return u64_; }

    [[nodiscard]] TypeId internName(std::string_view name, TypeKind kind);
    [[nodiscard]] TypeId internPointer(TypeId pointee);
    [[nodiscard]] TypeId internOptional(TypeId inner);
    [[nodiscard]] TypeId internArray(TypeId element, std::uint64_t length);
    [[nodiscard]] TypeId internSlice(TypeId element);
    [[nodiscard]] TypeId internFunction(
        common::memory::DynArray<TypeId> &params, TypeId result);
    [[nodiscard]] TypeId internStruct(
        std::string_view name, common::memory::DynArray<TypeId> &fields,
        common::memory::DynArray<std::string_view> &names);
    [[nodiscard]] TypeId internEnum(
        std::string_view name, TypeId underlying,
        common::memory::DynArray<std::string_view> &variantNames,
        common::memory::DynArray<std::int64_t> &discriminants);
    [[nodiscard]] TypeId internUnion(
        std::string_view name, common::memory::DynArray<TypeId> &members);
    [[nodiscard]] TypeId internTrait(std::string_view name);
    [[nodiscard]] TypeId internAlias(TypeId target);
    [[nodiscard]] TypeId internNominal(std::string_view name, TypeId target);
    [[nodiscard]] TypeId internGeneric(std::string_view name);
    [[nodiscard]] TypeId internPlaceholder(std::string_view name);
    [[nodiscard]] TypeId internQualified(
        TypeId inner, Ownership ownership, bool isMut);

    void registerNamed(std::string_view name, TypeId id);
    [[nodiscard]] TypeId lookupNamed(std::string_view name) const noexcept;
    [[nodiscard]] TypeId findOrCreateNamed(std::string_view name,
                                           TypeKind kind);

    [[nodiscard]] const TypeDesc *find(TypeId id) const noexcept;
    [[nodiscard]] TypeKind kindOf(TypeId id) const noexcept;
    [[nodiscard]] bool isInteger(TypeId id) const noexcept;
    [[nodiscard]] bool isFloat(TypeId id) const noexcept;
    [[nodiscard]] bool isNumeric(TypeId id) const noexcept;
    [[nodiscard]] const TypeDesc *qualified(TypeId id) const noexcept;

    /// Strips qualifiers, aliases, and named placeholders, returning the
    /// concrete type used for unification.
    [[nodiscard]] TypeId resolve(TypeId id) const noexcept;

    [[nodiscard]] std::string toString(TypeId id) const;

    [[nodiscard]] int fieldIndex(TypeId structType,
                                 std::string_view name) const noexcept;

    [[nodiscard]] TypeId lowerTypeExpr(const generated_ast::TypeExpr *type);

    common::memory::DynArray<TypeId> &makeTypeStorage();
    common::memory::DynArray<std::string_view> &makeNameStorage();
    common::memory::DynArray<std::int64_t> &makeDiscStorage();

private:
    common::memory::Arena &arena_;
    type_system::TypeContext typeContext_;
    common::memory::DynArray<TypeDesc> entries_;
    common::memory::FlatMap<std::string_view, TypeId> named_;

    TypeId error_ = kInvalidTypeId;
    TypeId invalid_ = kInvalidTypeId;
    TypeId null_ = kInvalidTypeId;
    TypeId void_ = kInvalidTypeId;
    TypeId bool_ = kInvalidTypeId;
    TypeId char_ = kInvalidTypeId;
    TypeId i32_ = kInvalidTypeId;
    TypeId i64_ = kInvalidTypeId;
    TypeId f32_ = kInvalidTypeId;
    TypeId f64_ = kInvalidTypeId;
    TypeId u64_ = kInvalidTypeId;

    [[nodiscard]] TypeId push(TypeKind kind, std::string_view name);
    [[nodiscard]] TypeId registerBuiltinFromContext(
        std::string_view name, TypeKind kind);
    [[nodiscard]] TypeId lowerBareTypeExpr(
        const generated_ast::TypeExpr *type, bool &reported);
};

} // namespace toolkit::sema
