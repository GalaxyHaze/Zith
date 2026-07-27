#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/result.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace zith::symbols {

class DepGraph {
public:
    class ResolveGuard {
    public:
        ResolveGuard() = default;
        ResolveGuard(memory::FlatMap<std::string, char> *resolving, std::string key);
        ResolveGuard(ResolveGuard &&other) noexcept;
        auto operator=(ResolveGuard &&other) noexcept -> ResolveGuard &;
        ResolveGuard(const ResolveGuard &)                     = delete;
        auto operator=(const ResolveGuard &) -> ResolveGuard & = delete;
        ~ResolveGuard();

    private:
        memory::FlatMap<std::string, char> *resolving_ = nullptr;
        std::string key_;
    };

    explicit DepGraph(memory::Arena &arena);

    auto loadedIndex(std::string_view import_key) const -> const size_t *;
    auto loadedByCanonical(std::string_view canonical_path) const -> const size_t *;
    auto beginResolve(const std::string &import_key) -> memory::Result<ResolveGuard>;
    auto beginResolveCanonical(const std::string &canonical_path) -> memory::Result<ResolveGuard>;
    void remember(std::string_view import_key, std::string_view canonical_path, size_t idx);
    bool isLoaded(std::string_view import_key) const;

    void setRootDeps(memory::DynArray<size_t> deps);
    const memory::DynArray<size_t> &rootDeps() const;

private:
    memory::FlatMap<std::string, size_t> index_by_path_;
    memory::FlatMap<std::string, size_t> index_by_canonical_;
    memory::FlatMap<std::string, char> resolving_;
    memory::FlatMap<std::string, char> resolving_canonical_;
    memory::DynArray<size_t> root_deps_;
};

struct ModuleDependencies {
    memory::DynArray<size_t> re_exported_files;
    memory::DynArray<size_t> dep_files;

    explicit ModuleDependencies(memory::Arena &arena)
        : re_exported_files(arena), dep_files(arena) {}
};

using ResolveImportFn = std::function<memory::Result<size_t>(
    const memory::DynArray<std::string_view> &path,
    const memory::DynArray<ast::ImportSymbol> &symbols, bool is_from, bool is_export, bool is_asset,
    std::string_view alias, int32_t import_depth, std::string_view source_file)>;

auto collectModuleDependencies(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               ast::AstBuilder &builder, const ast::ProgramNode &program,
                               std::string_view source_file, ResolveImportFn resolve_import)
    -> ModuleDependencies;

} // namespace zith::symbols
