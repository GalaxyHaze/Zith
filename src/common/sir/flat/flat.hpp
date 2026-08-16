#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/result.hpp"
#include "common/memory/string-interner.hpp"
#include "common/sir/sir.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace toolkit::sir::flat {

using common::memory::Arena;
using common::memory::DynArray;
using common::memory::InternedId;
using common::memory::Result;
using common::memory::StringInterner;

constexpr std::uint32_t invalidIndex = 0xFFFFFFFFu;

enum class FlatOp : std::uint8_t {
    Constant,
    Param,
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Load,
    Store,
    Call,
    Return,
    Branch,
    CondBranch,
};

struct FlatType {
    TypeKind kind = TypeKind::Void;
    InternedId nameId = invalidIndex;
    std::uint32_t elementType = invalidIndex;
    std::uint32_t pointeeType = invalidIndex;
    std::uint64_t arrayLength = 0;
};

struct FlatTerminator {
    FlatOp op = FlatOp::Return;
    std::uint32_t value = invalidIndex;
    std::uint32_t variable = invalidIndex;
    std::uint32_t condition = invalidIndex;
    std::uint32_t trueTarget = invalidIndex;
    std::uint32_t falseTarget = invalidIndex;
};

struct FlatBlock {
    DynArray<std::uint32_t> *valueIndices = nullptr;
    FlatTerminator terminator;
};

struct FlatValue {
    FlatOp op = FlatOp::Constant;
    std::uint32_t typeIndex = invalidIndex;
    std::uint32_t variableIndex = invalidIndex;
    std::uint32_t left = invalidIndex;
    std::uint32_t right = invalidIndex;
    std::uint32_t address = invalidIndex;
    std::uint32_t calleeFunction = invalidIndex;
    DynArray<std::uint32_t> *args = nullptr;
    std::int64_t intValue = 0;
    double floatValue = 0.0;
};

struct FlatVariable {
    InternedId name = invalidIndex;
    std::uint32_t type = invalidIndex;
    Mutability mutability = Mutability::Mutable;
};

struct FlatFunction {
    InternedId name = invalidIndex;
    std::uint32_t returnType = invalidIndex;
    DynArray<std::uint32_t> *paramTypes = nullptr;
    DynArray<InternedId> *paramNames = nullptr;
    DynArray<FlatVariable> *variables = nullptr;
    DynArray<FlatBlock> *blocks = nullptr;
    DynArray<FlatValue> *values = nullptr;
    std::uint32_t baseBlock = invalidIndex;
};

struct FlatModule {
    Arena *arena = nullptr;
    StringInterner *interner = nullptr;
    InternedId name = invalidIndex;
    DynArray<FlatType> types;
    DynArray<FlatFunction *> functions;

    explicit FlatModule(Arena &allocator)
        : arena(&allocator),
          interner(allocator.make<StringInterner>(allocator)),
          name(invalidIndex),
          types(allocator),
          functions(allocator) {}
};

[[nodiscard]] Result<FlatModule> flattenModule(Module &module);
[[nodiscard]] Result<void> verify(const FlatModule &module);
[[nodiscard]] Result<std::vector<std::uint8_t>> serializeFlatModule(const FlatModule &module);
[[nodiscard]] Result<FlatModule> deserializeFlatModule(Arena &arena, std::string_view bytes);

} // namespace toolkit::sir::flat
