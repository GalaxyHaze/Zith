#include "import-manager.hpp"
#include "import-resolver.hpp"
#include "loaded-file-factory.hpp"
#include "module-loader.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace zith::symbols {
namespace fs = std::filesystem;

ImportManager::ImportManager(memory::Arena &arena, memory::StringInterner &interner,
                             memory::SourceMap &source_map, diagnostics::DiagnosticEngine &diags,
                             std::vector<std::string> visible_roots)
    : arena_(arena), interner_(interner), source_map_(source_map), diags_(diags),
      visible_roots_(std::move(visible_roots)), dep_graph_(arena), registry_(arena) {}

bool ImportManager::isLoaded(std::string_view path) const {
    return dep_graph_.isLoaded(path);
}

const LoadedFile &ImportManager::get(size_t idx) const {
    return registry_.get(idx);
}

static std::string joinWith(const memory::DynArray<std::string_view> &path, char sep) {
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0)
            result += sep;
        result.append(path[i].data(), path[i].size());
    }
    return result;
}

static std::string canonicalPath(const std::string &path) {
    return fs::weakly_canonical(fs::path(path)).string();
}

auto ImportManager::resolve_file(const std::string &full_path, const std::string &import_key,
                                 const std::string &ns, bool is_from, bool is_export,
                                 const std::string &alias, int32_t import_depth,
                                 const memory::DynArray<ast::ImportSymbol> &symbols)
    -> memory::Result<size_t> {
    if (auto *existing = dep_graph_.loadedIndex(import_key))
        return *existing;

    auto canonical_path = canonicalPath(full_path);
    if (auto *existing = dep_graph_.loadedByCanonical(canonical_path)) {
        dep_graph_.remember(import_key, canonical_path, *existing);
        return *existing;
    }

    auto guard = dep_graph_.beginResolveCanonical(canonical_path);
    if (!guard)
        return std::move(guard.error());
    auto resolve_guard = std::move(guard.value());

    // ── C/C++ header: don't tokenize, just register ───────────────
    auto ext         = fs::path(full_path).extension();
    bool is_c_header = (ext == ".h" || ext == ".hpp");

    if (is_c_header) {
        auto idx = registry_.add(loaded_file_factory::makeHeaderFile(
            arena_, interner_, full_path, import_key, ns, is_from, is_export, alias, import_depth));
        dep_graph_.remember(import_key, canonical_path, idx);
        return idx;
    }

    ModuleLoader loader(arena_, interner_, source_map_, diags_);
    auto loaded = loader.load(full_path);
    if (!loaded)
        return std::move(loaded.error());

    auto module      = std::move(loaded.value());
    auto file_id     = module.file_id;
    auto syms        = std::move(module.symbols);
    auto *builder    = module.builder;
    auto program     = std::move(module.program);
    auto public_syms = std::move(module.public_syms);
    auto module_syms = std::move(module.module_syms);

    // ── Handle re-exports and from-imports ─────────────────────────
    auto deps = collectModuleDependencies(
        arena_, diags_, *builder, program, full_path,
        [&](const memory::DynArray<std::string_view> &child_path,
            const memory::DynArray<ast::ImportSymbol> &child_symbols, bool child_is_from,
            bool child_is_export, bool child_is_asset, std::string_view child_alias,
            int32_t child_import_depth, std::string_view child_source_file) {
            return resolve(child_path, child_symbols, child_is_from, child_is_export,
                           child_is_asset, child_alias, child_import_depth, child_source_file);
        });

    auto idx = registry_.add(loaded_file_factory::makeImportedFile(arena_, import_key, ns, is_from,
                                                                   is_export, alias, import_depth,
                                                                   LoadedModule{
                                                                       file_id,
                                                                       std::move(syms),
                                                                       builder,
                                                                       std::move(program),
                                                                       std::move(public_syms),
                                                                       std::move(module_syms),
                                                                   },
                                                                   std::move(deps), symbols));
    dep_graph_.remember(import_key, canonical_path, idx);
    return idx;
}

auto ImportManager::resolve_directory(const std::string &import_key, const std::string &dir_path,
                                      const memory::DynArray<std::string_view> &path, bool is_from,
                                      bool is_export, const std::string &alias,
                                      int32_t import_depth) -> memory::Result<size_t> {
    auto planned = import_resolver::planDirectoryImports(import_key, dir_path, path, alias, is_from,
                                                         import_depth);
    if (!planned)
        return std::move(planned.error());

    size_t first_idx = SIZE_MAX;

    for (auto &entry : planned.value()) {
        if (auto *existing = dep_graph_.loadedIndex(entry.import_key)) {
            if (first_idx == SIZE_MAX)
                first_idx = *existing;
            continue;
        }

        auto res = resolve_file(entry.full_path, entry.import_key, entry.ns, is_from, is_export,
                                alias, import_depth, memory::DynArray<ast::ImportSymbol>(arena_));
        if (!res)
            return std::move(res.error());

        if (first_idx == SIZE_MAX)
            first_idx = res.value();
    }

    if (first_idx == SIZE_MAX)
        return memory::Error{"directory '" + dir_path + "' contains no resolvable imports"};

    // The directory key aliases its first file only to deduplicate repeated directory imports.
    dep_graph_.remember(import_key, canonicalPath(dir_path), first_idx);
    return first_idx;
}

auto ImportManager::resolve_asset_file(const std::string &full_path, const std::string &import_key,
                                       const std::string &alias) -> memory::Result<size_t> {
    if (auto *existing = dep_graph_.loadedIndex(import_key))
        return *existing;

    auto canonical_path = canonicalPath(full_path);
    if (auto *existing = dep_graph_.loadedByCanonical(canonical_path)) {
        dep_graph_.remember(import_key, canonical_path, *existing);
        return *existing;
    }

    auto idx = registry_.add(
        loaded_file_factory::makeAssetFile(arena_, interner_, full_path, import_key, alias));
    dep_graph_.remember(import_key, canonical_path, idx);
    return idx;
}

auto ImportManager::resolve(const memory::DynArray<std::string_view> &path,
                            const memory::DynArray<ast::ImportSymbol> &symbols, bool is_from,
                            bool is_export, bool is_asset, std::string_view alias,
                            int32_t import_depth, std::string_view source_file)
    -> memory::Result<size_t> {

    auto import_key = joinWith(path, '/');

    // ── Dedup ───────────────────────────────────────────────────────
    if (auto *existing = dep_graph_.loadedIndex(import_key))
        return *existing;

    // ── Cycle detection ────────────────────────────────────────────
    auto guard = dep_graph_.beginResolve(import_key);
    if (!guard)
        return std::move(guard.error());
    auto resolve_guard = std::move(guard.value());

    // ── Asset file ─────────────────────────────────────────────────
    if (is_asset) {
        auto asset_path = import_resolver::findAssetFile(import_key, asset_roots_);
        if (asset_path)
            return resolve_asset_file(*asset_path, import_key, std::string(alias));
        return memory::Error{"could not resolve asset '" + import_key + "'"};
    }

    auto file_path = import_resolver::findFile(import_key, source_file, visible_roots_);
    if (!file_path) {
        std::string msg = "could not resolve import '" + import_key + "'";
        if (!visible_roots_.empty()) {
            msg += " — searched roots:";
            for (auto &r : visible_roots_)
                msg += "\n    " + r;
        }
        if (source_file.empty())
            msg += "\n    (no source file context for relative resolution)";
        else
            msg += "\n    (relative to: " + std::string(source_file) + ")";
        return memory::Error{std::move(msg)};
    }

    // ── Directory import ───────────────────────────────────────────
    if (fs::is_directory(*file_path))
        return resolve_directory(import_key, *file_path, path, is_from, is_export,
                                 std::string(alias), import_depth);

    // ── Single file resolution ─────────────────────────────────────
    auto ns = import_resolver::computeNamespace(path, alias);

    return resolve_file(*file_path, import_key, ns, is_from, is_export, std::string(alias),
                        import_depth, symbols);
}

void ImportManager::mergeInto(SymbolTable &main_syms, int32_t from_depth) {
    registry_.mergeInto(arena_, interner_, diags_, main_syms, from_depth);
}

memory::Optional<SymOrigin> ImportManager::originOf(SymId main_sym) const {
    return registry_.originOf(main_sym);
}

void ImportManager::setVisibleRoots(std::vector<std::string> roots) {
    visible_roots_ = std::move(roots);
}

void ImportManager::setAssetRoots(std::vector<std::string> roots) {
    asset_roots_ = std::move(roots);
}

} // namespace zith::symbols
