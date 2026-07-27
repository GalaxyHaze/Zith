#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"
#include "symbols/symbol-table.hpp"

#include <string>

namespace zith::symbols {

struct LoadedFile {
    std::string import_key;
    std::string ns;
    bool is_from   = false;
    bool is_export = false;
    std::string alias;
    int32_t import_depth = 1;
    memory::FileId file_id;
    SymbolTable symbols;
    ast::AstBuilder *builder;
    ast::ProgramNode program;
    memory::DynArray<SymId> public_syms;
    memory::DynArray<SymId> module_syms;
    memory::DynArray<size_t> re_exported_files;
    memory::DynArray<size_t> dep_files;
    memory::DynArray<ast::ImportSymbol> import_symbols;
    bool is_asset    = false;
    bool is_c_header = false;
    std::string header_path;
};

struct SymOrigin {
    size_t file_idx;
    SymId local_sym;
};

} // namespace zith::symbols
