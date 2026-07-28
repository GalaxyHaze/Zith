#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "memory/arena.hpp"
#include "memory/optional.hpp"
#include "memory/result.hpp"
#include "memory/source-map.hpp"
#include "symbols/dep-graph.hpp"
#include "symbols/import-registry.hpp"
#include "symbols/import-types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace zith::symbols {

class ImportManager {
public:
    ImportManager(memory::Arena &arena, memory::StringInterner &interner,
                  memory::SourceMap &source_map, diagnostics::DiagnosticEngine &diags,
                  std::vector<std::string> visible_roots = {});

    auto resolve(const memory::DynArray<std::string_view> &path,
                 const memory::DynArray<ast::ImportSymbol> &symbols, bool is_from = false,
                 bool is_export = false, bool is_asset = false, std::string_view alias = {},
                 int32_t import_depth = 1, std::string_view source_file = {})
        -> memory::Result<size_t>;

    void mergeInto(SymbolTable &main_syms, int32_t from_depth = 0);

    const LoadedFile &get(size_t idx) const;
    size_t fileCount() const noexcept {
        return registry_.fileCount();
    }

    bool isLoaded(std::string_view path) const;
    memory::Optional<SymOrigin> originOf(SymId main_sym) const;
    void setRootDeps(memory::DynArray<size_t> deps) {
        dep_graph_.setRootDeps(std::move(deps));
    }
    const memory::DynArray<size_t> &rootDeps() const {
        return dep_graph_.rootDeps();
    }
    void setVisibleRoots(std::vector<std::string> roots);
    void setAssetRoots(std::vector<std::string> roots);

private:
    memory::Arena &arena_;
    memory::StringInterner &interner_;
    memory::SourceMap &source_map_;
    diagnostics::DiagnosticEngine &diags_;
    std::vector<std::string> visible_roots_;
    std::vector<std::string> asset_roots_;
    DepGraph dep_graph_;
    ImportRegistry registry_;

    auto resolve_file(const std::string &full_path, const std::string &import_key,
                      const std::string &ns, bool is_from, bool is_export, const std::string &alias,
                      int32_t import_depth, const memory::DynArray<ast::ImportSymbol> &symbols)
        -> memory::Result<size_t>;

    auto resolve_directory(const std::string &import_key, const std::string &dir_path,
                           const memory::DynArray<std::string_view> &path, bool is_from,
                           bool is_export, const std::string &alias, int32_t import_depth)
        -> memory::Result<size_t>;

    auto resolve_asset_file(const std::string &full_path, const std::string &import_key,
                            const std::string &alias) -> memory::Result<size_t>;
};

} // namespace zith::symbols
