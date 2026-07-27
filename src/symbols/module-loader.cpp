#include "module-loader.hpp"

#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/parser.hpp"

namespace zith::symbols {

ModuleLoader::ModuleLoader(memory::Arena &arena, memory::StringInterner &interner,
                           memory::SourceMap &source_map, diagnostics::DiagnosticEngine &diags)
    : arena_(arena), interner_(interner), source_map_(source_map), diags_(diags) {}

auto ModuleLoader::load(const std::string &full_path) -> memory::Result<LoadedModule> {
    auto file_result = source_map_.loadFile(full_path);
    if (!file_result)
        return memory::Error{"failed to load '" + full_path + "'"};

    auto file_id      = file_result.value();
    auto token_result = lexer::tokenize(source_map_, arena_, file_id, diags_);
    if (!token_result)
        return memory::Error{"failed to tokenize '" + full_path + "'"};

    lexer::TokenStream tokens = std::move(token_result.value());
    auto *builder             = arena_.make<ast::AstBuilder>(arena_, interner_);
    SymbolTable symbols(arena_, &interner_);
    parser::Parser parser(&tokens, builder, &diags_);
    auto scan_result = parser::scan(parser, symbols);
    parser.expandBodies(scan_result, symbols);

    memory::DynArray<SymId> public_syms{arena_};
    memory::DynArray<SymId> module_syms{arena_};

    for (SymId id = 0; id < static_cast<SymId>(symbols.symbolCount()); ++id) {
        auto &data = symbols.get(id);
        if (data.visibility == SymbolVisibility::Public)
            public_syms.push(id);
        else if (data.visibility == SymbolVisibility::Module)
            module_syms.push(id);
    }

    return LoadedModule{
        file_id,
        std::move(symbols),
        builder,
        std::move(parser.program),
        std::move(public_syms),
        std::move(module_syms),
    };
}

} // namespace zith::symbols
