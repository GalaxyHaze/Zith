#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/result.hpp"
#include "common/sir/sir.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace toolkit::codegen {

using common::memory::Arena;
using common::memory::DynArray;
using common::memory::Error;
using common::memory::Result;

enum class Op : uint8_t {
    ConstI32,
    ConstI64,
    ConstF64,
    AddI32,
    AddI64,
    AddF64,
    SubI32,
    SubI64,
    SubF64,
    MulI32,
    MulI64,
    MulF64,
    Load,
    Store,
    RetI32,
    RetI64,
    RetF64,
    RetVoid,
};

struct Instruction {
    Op op = Op::RetVoid;
    std::size_t slot = 0;
    std::size_t reg = 0;
    std::size_t local = 0;
    std::size_t src = 0;
    std::size_t dst = 0;
    std::size_t width = 0;
    std::int64_t constant = 0;
};

struct FunctionCode {
    Arena *arena;
    std::string_view name;
    std::size_t paramBytes = 0;
    std::size_t localBytes = 0;
    DynArray<Instruction> instructions;

    explicit FunctionCode(Arena &arena_)
        : arena(&arena_), instructions(arena_) {}
};

struct ByteCode {
    Arena *arena = nullptr;
    DynArray<FunctionCode> functions;

    explicit ByteCode(Arena &arena_) : arena(&arena_), functions(arena_) {}
};

enum class Backend {
    VM,
};

class CodegenState {
public:
    explicit CodegenState(Arena &arena_) : arena_(&arena_) {}

    [[nodiscard]] Result<ByteCode> flatten(toolkit::sir::Module &module);
    [[nodiscard]] Result<ByteCode> runBackend(toolkit::sir::Module &module,
                                              Backend backend = Backend::VM);

private:
    Arena *arena_ = nullptr;
};

[[nodiscard]] Result<ByteCode> codegen(toolkit::sir::Module &module);

} // namespace toolkit::codegen
