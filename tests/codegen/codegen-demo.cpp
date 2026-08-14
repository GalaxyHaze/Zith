#include "codegen/codegen.hpp"
#include "codegen/vm/vm.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/result.hpp"
#include "common/sir/sir.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>

using common::memory::Arena;
using toolkit::codegen::CodegenState;
using toolkit::codegen::vm::StdReturn;
using toolkit::codegen::vm::VM;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;

namespace {

std::int32_t decodeI32(const StdReturn &result) {
    std::int32_t value = 0;
    std::memcpy(&value, result.raw, sizeof(value));
    return value;
}

std::array<std::uint8_t, 8> i32Args(std::int32_t left, std::int32_t right) {
    std::array<std::uint8_t, 8> args{};
    std::memcpy(args.data(), &left, sizeof(left));
    std::memcpy(args.data() + sizeof(left), &right, sizeof(right));
    return args;
}

} // namespace

int main() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("codegen-demo");

    auto &add = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    add.ret(add.add(add.param(0), add.param(1)));

    auto bytecode = CodegenState{arena}.runBackend(module, toolkit::codegen::Backend::VM);
    if (!bytecode.isOk()) {
        std::cerr << "codegen-demo: " << bytecode.error().msg << '\n';
        return EXIT_FAILURE;
    }

    VM vm;
    const auto args = i32Args(3, 4);
    auto &fn = bytecode.value().functions[0];
    const auto result = vm.run(fn, std::span<const std::uint8_t>{args});
    const std::int32_t decoded = decodeI32(result);

    std::cout << "codegen-demo: add(3, 4) returned " << decoded << "\n";
    return decoded == 7 ? EXIT_SUCCESS : EXIT_FAILURE;
}
