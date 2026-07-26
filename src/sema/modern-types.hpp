#pragma once

#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"

#include <cstdint>
#include <string_view>

namespace zith::sema::modern {

/// Stable identifier for concrete types shared by sema, solve and NRA.
/// `intern_seq` is monotonic and unique within a `TypeTable`.
struct TypeId {
    uint32_t intern_seq = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return intern_seq != 0;
    }
    friend constexpr bool operator==(TypeId, TypeId) = default;
};

inline constexpr TypeId kInvalidTypeId{};

enum class TypeKind : uint8_t { Error, Void, Never, Bool, Char, Integer, Float, String,
                                Pointer, Optional, Array, Function, Struct, Unknown };

struct IntegerType { uint8_t bits = 0; bool isSigned = true; };
struct FloatType   { uint8_t bits = 0; };
struct PointerType { TypeId pointee; };
struct OptionalType { TypeId inner; };
struct ArrayType    { TypeId element; uint64_t size = 0; };
struct FunctionType { memory::DynArray<TypeId> &params; TypeId result; };
struct StructType   { std::string_view name; memory::DynArray<TypeId> &fields; };

/// Append-only type table shared by sema, solve and NRA.
class TypeTable {
public:
    explicit TypeTable(memory::Arena &arena);

    [[nodiscard]] TypeId internName(std::string_view name, TypeKind kind = TypeKind::Unknown);
    [[nodiscard]] TypeId internInteger(IntegerType int_type);
    [[nodiscard]] TypeId internFloat(FloatType float_type);
    [[nodiscard]] TypeId internPointer(TypeId pointee);
    [[nodiscard]] TypeId internOptional(TypeId inner);
    [[nodiscard]] TypeId internArray(TypeId element, uint64_t size);
    [[nodiscard]] TypeId internFunction(memory::DynArray<TypeId> &params, TypeId result);
    [[nodiscard]] TypeId internStruct(std::string_view name, memory::DynArray<TypeId> &fields);

    [[nodiscard]] TypeKind kindOf(TypeId id) const noexcept;

    [[nodiscard]] const PointerType  *pointer(TypeId id) const noexcept;
    [[nodiscard]] const OptionalType *optional(TypeId id) const noexcept;
    [[nodiscard]] const ArrayType    *array(TypeId id) const noexcept;
    [[nodiscard]] const IntegerType  *integer(TypeId id) const noexcept;
    [[nodiscard]] const FloatType    *float_kind(TypeId id) const noexcept;
    [[nodiscard]] const FunctionType *function(TypeId id) const noexcept;
    [[nodiscard]] const StructType   *struct_type(TypeId id) const noexcept;

    [[nodiscard]] size_t size() const noexcept;

private:
    enum class EntryKind : uint8_t { Name, Integer, Float, Pointer, Optional, Array, Function, Struct };

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
        FunctionType *fn = nullptr;
        StructType *struct_ty = nullptr;
        memory::DynArray<TypeId> *storage = nullptr;
    };

    memory::DynArray<Entry> entries_;
    memory::Arena *arena_;
    uint32_t next_seq_ = 1;

    Entry &pushEntry(EntryKind kind);
    memory::DynArray<TypeId> &makeStorage();
    const Entry *findEntry(TypeId id) const noexcept;
};

} // namespace zith::sema::modern
