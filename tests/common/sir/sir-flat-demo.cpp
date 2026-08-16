#include "common/sir/flat/flat.hpp"
#include "common/sir/sir.hpp"

#include "common/memory/arena.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using common::memory::Arena;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;

int main() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("sir-flat-demo");
    auto &fn = module.declareFn("sum", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});
    fn.ret(fn.add(fn.param(0), fn.param(1)));

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (flat.isError()) {
        std::fprintf(stderr, "flatten failed: %s\n", flat.error().msg.c_str());
        return EXIT_FAILURE;
    }
    auto bytes = toolkit::sir::flat::serializeFlatModule(flat.value());
    if (bytes.isError()) {
        std::fprintf(stderr, "serialize failed: %s\n", bytes.error().msg.c_str());
        return EXIT_FAILURE;
    }

    Arena roundtripArena;
    auto decoded = toolkit::sir::flat::deserializeFlatModule(
        roundtripArena,
        std::string_view(reinterpret_cast<const char *>(bytes.value().data()),
                         bytes.value().size()));
    if (decoded.isError()) {
        std::fprintf(stderr, "deserialize failed: %s\n", decoded.error().msg.c_str());
        return EXIT_FAILURE;
    }

    const auto &flatModule = flat.value();
    const auto &roundtripModule = decoded.value();
    std::printf("sir-flat-demo: module=%s functions=%zu types=%zu bytes=%zu "
                "roundtripFunctions=%zu\n",
                roundtripModule.interner->lookup(roundtripModule.name).data(),
                flatModule.functions.size(), flatModule.types.size(), bytes.value().size(),
                roundtripModule.functions.size());
    return EXIT_SUCCESS;
}
