#include "common/diagnostic/diagnostic.hpp"
#include "common/diagnostic/render.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/result.hpp"
#include "common/memory/source-map.hpp"
#include "common/memory/string-interner.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::diagnostic::Diagnostic;
using common::diagnostic::renderDiagnostic;
using common::memory::Arena;
using common::memory::DynArray;
using common::memory::Result;
using common::memory::SourceMap;
using common::memory::SourceSpan;
using common::memory::Span;
using common::memory::StringInterner;

namespace {

Result<int> checkedValue(int value) {
    return value >= 0 ? Result<int>{value} : Result<int>{common::memory::Error{"negative"}};
}

} // namespace

int main() {
    Arena arena;
    DynArray<int> values(arena);
    values.push(1);
    values.push(2);
    values.push(3);

    StringInterner strings(arena);
    const auto hello = strings.intern("hello");
    const auto world = strings.intern("world");

    const auto ok = checkedValue(10);
    const auto bad = checkedValue(-1);

    SourceMap map;
    const auto file = map.addFile("common-demo.zith", "let x = 0;\n");
    if (!file.isOk() || !ok.isOk() || bad.isOk()) {
        std::cerr << "common-demo: API validation failed\n";
        return EXIT_FAILURE;
    }

    Diagnostic diag;
    diag.span = SourceSpan{file.value(), Span{4, 5}};
    diag.message = "demo diagnostic";
    renderDiagnostic(stdout, map, diag, {});

    std::cout << "arena.allocatedBytes=" << arena.allocatedBytes() << "\n"
              << "dynArray.size=" << values.size()
              << " sum=" << (values[0] + values[1] + values[2]) << "\n"
              << "interned.hello=" << strings.lookup(hello) << "\n"
              << "interned.world=" << strings.lookup(world) << "\n"
              << "result.ok=" << ok.value() << "\n"
              << "result.bad.isError=" << (bad.isError() ? "true" : "false") << "\n"
              << "sourceMap.snippet=" << map.snippet({file.value(), Span{4, 5}}).value() << "\n";

    std::cout << "common-demo: arena, DynArray, interner, Result, SourceMap, render passed\n";
    return EXIT_SUCCESS;
}
