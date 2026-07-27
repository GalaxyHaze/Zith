#pragma once

#include "common/ast-ids.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"

#include <string_view>

namespace zith::parser {

struct ScanEntry {
    std::string_view name;
    memory::Span span;
    ast::ExprId body_node;
    uint32_t struct_header_start = 0;
};

struct ScanResult {
    memory::DynArray<ScanEntry> fns;
    memory::DynArray<ScanEntry> structs;
    memory::DynArray<ScanEntry> unions;
    memory::DynArray<ScanEntry> enums;
    memory::DynArray<ScanEntry> components;
    memory::DynArray<ScanEntry> traits;
    memory::DynArray<ScanEntry> words;
    memory::DynArray<ScanEntry> contexts;

    explicit ScanResult(memory::Arena &arena)
        : fns(arena), structs(arena), unions(arena), enums(arena), components(arena), traits(arena),
          words(arena), contexts(arena) {}
};

} // namespace zith::parser
