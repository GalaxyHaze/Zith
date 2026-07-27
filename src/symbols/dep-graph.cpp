#include "dep-graph.hpp"

#include "legacy-zith/ast/ast-node-utils.hpp"
#include "diagnostics/error-codes.hpp"

#include <utility>

namespace zith::symbols {

namespace {

std::string joinWith(const memory::DynArray<std::string_view> &path, char sep) {
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0)
            result += sep;
        result.append(path[i].data(), path[i].size());
    }
    return result;
}

} // namespace

DepGraph::ResolveGuard::ResolveGuard(memory::FlatMap<std::string, char> *resolving, std::string key)
    : resolving_(resolving), key_(std::move(key)) {}

DepGraph::ResolveGuard::ResolveGuard(ResolveGuard &&other) noexcept
    : resolving_(other.resolving_), key_(std::move(other.key_)) {
    other.resolving_ = nullptr;
}

auto DepGraph::ResolveGuard::operator=(ResolveGuard &&other) noexcept -> ResolveGuard & {
    if (this == &other)
        return *this;

    if (resolving_)
        resolving_->erase(key_);

    resolving_       = other.resolving_;
    key_             = std::move(other.key_);
    other.resolving_ = nullptr;
    return *this;
}

DepGraph::ResolveGuard::~ResolveGuard() {
    if (resolving_)
        resolving_->erase(key_);
}

DepGraph::DepGraph(memory::Arena &arena) : root_deps_(arena) {}

auto DepGraph::loadedIndex(std::string_view import_key) const -> const size_t * {
    return index_by_path_.get(import_key);
}

auto DepGraph::loadedByCanonical(std::string_view canonical_path) const -> const size_t * {
    return index_by_canonical_.get(canonical_path);
}

auto DepGraph::beginResolve(const std::string &import_key) -> memory::Result<ResolveGuard> {
    if (resolving_.contains(import_key))
        return memory::Error{"circular import detected: '" + import_key + "'"};

    resolving_.insert(import_key, char(1));
    return ResolveGuard(&resolving_, import_key);
}

auto DepGraph::beginResolveCanonical(const std::string &canonical_path)
    -> memory::Result<ResolveGuard> {
    if (resolving_canonical_.contains(canonical_path))
        return memory::Error{"circular import detected: '" + canonical_path + "'"};

    resolving_canonical_.insert(canonical_path, char(1));
    return ResolveGuard(&resolving_canonical_, canonical_path);
}

void DepGraph::remember(std::string_view import_key, std::string_view canonical_path, size_t idx) {
    index_by_path_.insert(std::string(import_key), idx);
    index_by_canonical_.insert(std::string(canonical_path), idx);
}

bool DepGraph::isLoaded(std::string_view import_key) const {
    return index_by_path_.contains(import_key) || resolving_.contains(import_key);
}

void DepGraph::setRootDeps(memory::DynArray<size_t> deps) {
    root_deps_ = std::move(deps);
}

const memory::DynArray<size_t> &DepGraph::rootDeps() const {
    return root_deps_;
}

auto collectModuleDependencies(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               ast::AstBuilder &builder, const ast::ProgramNode &program,
                               std::string_view source_file, ResolveImportFn resolve_import)
    -> ModuleDependencies {
    ModuleDependencies deps(arena);

    for (auto decl_id : program.decls) {
        auto &decl   = builder.getDecl(decl_id);
        auto *import = ast::asImport(decl);
        if (!import)
            continue;

        auto resolve = [&]() {
            return resolve_import(import->path, import->symbols, import->is_from, import->is_export,
                                  import->is_asset, import->alias, import->import_depth,
                                  source_file);
        };

        if (import->is_export) {
            auto re_res = resolve();
            if (re_res) {
                deps.re_exported_files.push(re_res.value());
            } else {
                auto key = joinWith(import->path, '/');
                diags.report(diagnostics::Severity::Error, diagnostics::err::ImportError,
                             "re-export of '" + key + "' failed: " + re_res.error().msg, {});
            }
            continue;
        }

        if (import->is_from) {
            auto dep_res = resolve();
            if (dep_res) {
                deps.dep_files.push(dep_res.value());
            } else {
                auto key = joinWith(import->path, '/');
                diags.report(diagnostics::Severity::Error, diagnostics::err::ImportError,
                             "from-import of '" + key + "' failed: " + dep_res.error().msg, {});
            }
        }
    }

    return deps;
}

} // namespace zith::symbols
