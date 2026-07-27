#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/result.hpp"
#include "memory/source-map.hpp"
#include "symbols/symbol-table.hpp"

#include <string>

namespace zith::symbols {

struct LoadedModule {
    memory::FileId file_id;
    SymbolTable symbols;
    ast::AstBuilder *builder;
    ast::ProgramNode program;
    memory::DynArray<SymId> public_syms;
    memory::DynArray<SymId> module_syms;
};

class ModuleLoader {
public:
    ModuleLoader(memory::Arena &arena, memory::StringInterner &interner,
                 memory::SourceMap &source_map, diagnostics::DiagnosticEngine &diags);

    auto load(const std::string &full_path) -> memory::Result<LoadedModule>;

private:
    memory::Arena &arena_;
    memory::StringInterner &interner_;
    memory::SourceMap &source_map_;
    diagnostics::DiagnosticEngine &diags_;
};

} // namespace zith::symbols
