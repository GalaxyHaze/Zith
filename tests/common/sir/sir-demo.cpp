#include "common/sir/sir.hpp"

#include "common/memory/arena.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::memory::Arena;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;

namespace {

bool expect(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "sir-demo: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("sir-demo");

    auto &function = module.declareFn(
        "sum", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    auto scope = function.pushScope();
    auto &total = scope.declVar("total", Primitives::i32);
    scope.store(total, function.add(function.param(0), function.param(1)));
    function.ret(total);

    const auto verification = toolkit::sir::verify(module);
    if (!expect(verification.isOk(), "module verifies"))
        return EXIT_FAILURE;

    std::cout << "sir-demo: module=" << module.nameView()
              << " functions=" << module.functions.size()
              << " params=" << function.params.size()
              << " instructions=" << function.instructions.size()
              << "\n";
    return EXIT_SUCCESS;
}
