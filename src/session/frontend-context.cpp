#include "session/frontend-context.hpp"
#include "session/frontend-context-internal.hpp"

#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace zith::session {

FrontendContext::FrontendContext(FrontendConfig config)
    : config_(std::move(config)), cache_key_(), catalog_(std::make_shared<SourceCatalog>()),
      cache_(), executor_(config_.maxFrontendWorkers) {
    applyConfig(config_);
    cache_key_ = config_.cacheKey();
}

FrontendMetrics FrontendContext::metrics() const {
    return {cache_.metrics(), executor_.workerCount()};
}

memory::Result<SourceCatalog::SourcePtr>
FrontendContext::sourceForPath(const std::string_view path) const {
    const auto canonical = SourceCatalog::canonicalPath(path);
    {
        std::shared_lock<std::shared_mutex> lock(overlay_mutex_);
        if (const auto *overlay = overlays_.get(canonical))
            return catalog_->registerSource(canonical, *overlay);
    }
    {
        std::shared_lock<std::shared_mutex> lock(overlay_mutex_);
        if (const auto *virtual_source = virtual_sources_.get(canonical))
            return catalog_->registerSource(canonical, *virtual_source);
    }
    return catalog_->loadFile(canonical);
}

void FrontendContext::registerVirtualSource(std::string path, std::string text) {
    const auto canonical = SourceCatalog::canonicalPath(path);
    {
        std::unique_lock<std::shared_mutex> lock(overlay_mutex_);
        virtual_sources_[canonical] = std::move(text);
    }
}

memory::Result<std::shared_ptr<const CompilationSnapshot>>
FrontendContext::analyzeFile(const std::string_view path) {
    auto source = sourceForPath(path);
    if (!source)
        return std::move(source.error());
    return analyze(std::move(source.value()));
}

memory::Result<std::shared_ptr<const CompilationSnapshot>>
FrontendContext::analyzeText(const std::string_view path, std::string text) {
    auto source = catalog_->registerSource(std::string(path), std::move(text));
    if (!source)
        return memory::Error{"source catalog is out of FileId values"};
    return analyze(std::move(source));
}

void FrontendContext::setOverlay(const std::string_view path, std::string text) {
    const auto canonical = SourceCatalog::canonicalPath(path);
    auto source          = catalog_->registerSource(canonical, text);
    {
        std::unique_lock<std::shared_mutex> lock(overlay_mutex_);
        overlays_[canonical] = std::move(text);
    }
    cache_.noteSource(source);
}

void FrontendContext::removeOverlay(const std::string_view path) {
    const auto canonical = SourceCatalog::canonicalPath(path);
    {
        std::unique_lock<std::shared_mutex> lock(overlay_mutex_);
        overlays_.erase(canonical);
    }
    cache_.invalidate(canonical);
}

memory::Result<std::shared_ptr<const CompilationSnapshot>>
FrontendContext::analyze(SourceCatalog::SourcePtr root_source) {
    if (!root_source)
        return memory::Error{"frontend analysis requires a root source"};
    const auto root_key      = root_source->canonicalPath;
    const auto visible_roots = visibleRootsFor(root_source->canonicalPath);
    std::map<ModuleKey, SourceCatalog::SourcePtr> pending;
    std::map<ModuleKey, ModuleArtifactPtr> modules;
    std::map<ModuleKey, std::vector<ModuleKey>> resolved_dependencies;
    std::vector<ImportEdge> import_graph;
    std::vector<ModuleDiagnostic> diagnostics;
    std::vector<ModuleDiagnostic> module_diagnostics;
    pending.emplace(root_key, std::move(root_source));

    while (!pending.empty()) {
        std::vector<PendingModule> batch;
        batch.reserve(pending.size());
        for (auto &[key, source] : pending) {
            if (modules.contains(key))
                continue;
            cache_.noteSource(source);
            batch.push_back({key, source});
        }
        pending.clear();

        std::vector<std::pair<ModuleKey, std::shared_future<ModuleArtifactPtr>>> futures;
        futures.reserve(batch.size());
        for (const auto &item : batch) {
            auto source = item.source;
            futures.emplace_back(item.key, cache_.getOrBuild(cache_key_, source, executor_,
                                                             [this, worker_source = source]() {
                                                                 return buildModule(worker_source);
                                                             }));
        }

        for (auto &[key, future] : futures) {
            auto module = future.get();
            if (!module)
                return memory::Error{"frontend worker failed to create '" + key + "'"};
            modules.emplace(key, std::move(module));
        }

        for (const auto &item : batch) {
            const auto module = modules.at(item.key);

            std::vector<ModuleKey> dependencies;
            for (const auto &request : module->imports) {
                const auto resolved = resolveImport(*module, request, visible_roots);
                ImportEdge edge;
                edge.importer   = module->key;
                edge.request    = request;
                edge.targets    = resolved.modules;
                edge.targetKind = resolved.targetKind;
                if (resolved.targetKind == ImportTargetKind::Zith && request.isHeader)
                    edge.request.isFrom = true;
                if (!resolved.found) {
                    const auto import_name = request.isAsset && !request.rawPath.empty()
                                                 ? request.rawPath
                                             : request.isHeader ? request.headerPath
                                                                : request.importKey();
                    if (request.isAsset) {
                        edge.error = "could not find asset '" + import_name +
                                     "'; check its path and configured asset roots";
                    } else if (request.isHeader) {
                        edge.error = "could not find header '" + import_name +
                                     "'; check its path and configured include roots";
                    } else if (resolved.consideredPlatformVariants) {
                        edge.error = "could not find module '" + import_name +
                                     "'; check its spelling, path, and configured import roots, "
                                     "or add a generic module or a platform variant matching "
                                     "the target";
                    } else {
                        edge.error = "could not find module '" + import_name +
                                     "'; check its spelling, path, and configured import roots";
                    }
                    diagnostics.push_back(makeImportDiagnostic(*module, request, edge.error));
                    import_graph.push_back(std::move(edge));
                    continue;
                }
                if (resolved.targetKind == ImportTargetKind::CppHeader) {
                    edge.error = "C++ headers are not supported in this version";
                    diagnostics.push_back(makeImportDiagnostic(*module, request, edge.error));
                    import_graph.push_back(std::move(edge));
                    continue;
                }
                if (resolved.targetKind == ImportTargetKind::CHeader) {
                    const auto &header_path = resolved.modules.front();
                    std::shared_ptr<const cinterop::CHeaderArtifact> header;
                    {
                        std::lock_guard<std::mutex> lock(c_headers_mutex_);
                        if (auto *cached = c_headers_by_path_.get(header_path))
                            header = *cached;
                    }
                    if (!header) {
                        cinterop::ParseOptions options;
                        options.targetTriple = config_.targetTriple;
                        options.sysroot      = config_.sysroot;
                        options.includeDirs  = config_.includeRoots;
                        for (const auto &root : config_.systemIncludeRoots)
                            options.includeDirs.push_back(root);
                        options.defines = config_.cDefines;
                        header          = cinterop::parseHeader(header_path, options);
                        std::lock_guard<std::mutex> lock(c_headers_mutex_);
                        auto *slot = c_headers_by_path_.get(header_path);
                        if (slot)
                            header = *slot;
                        else
                            c_headers_by_path_.insert(header_path, header);
                    }
                    edge.cHeader = header;
                    for (const auto &dependency : header->dependencies) {
                        auto source = catalog_->loadFile(dependency);
                        if (!source)
                            continue;
                        cache_.noteSource(source.value());
                        dependencies.push_back(source.value()->canonicalPath);
                    }
                    for (const auto &diagnostic : header->diagnostics) {
                        const auto location = diagnostic.line == 0U
                                                  ? ""
                                                  : " (" + header_path + ":" +
                                                        std::to_string(diagnostic.line) + ":" +
                                                        std::to_string(diagnostic.column) + ")";
                        diagnostics.push_back(makeImportDiagnostic(
                            *module, request,
                            "C header import failed: " + diagnostic.message + location));
                    }
                    import_graph.push_back(std::move(edge));
                    continue;
                }
                import_graph.push_back(std::move(edge));
                if (resolved.targetKind == ImportTargetKind::Asset)
                    continue;
                for (const auto &path : resolved.modules) {
                    auto source = sourceForPath(path);
                    if (!source) {
                        diagnostics.push_back(makeImportDiagnostic(*module, request,
                                                                   "failed to load import '" +
                                                                       request.importKey() +
                                                                       "': " + source.error().msg));
                        continue;
                    }
                    dependencies.push_back(source.value()->canonicalPath);
                    if (!modules.contains(source.value()->canonicalPath))
                        pending.emplace(source.value()->canonicalPath, std::move(source.value()));
                }
            }
            std::sort(dependencies.begin(), dependencies.end());
            dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                               dependencies.end());
            resolved_dependencies[item.key] = std::move(dependencies);
        }
    }

    for (const auto &[module, dependencies] : resolved_dependencies)
        cache_.updateDependencies(module, dependencies);

    std::vector<std::pair<ModuleKey, ModuleArtifactPtr>> rebuilt;
    rebuilt.reserve(modules.size());
    std::vector<ModuleArtifactPtr> initial_modules;
    initial_modules.reserve(modules.size());
    for (const auto &[key, module] : modules)
        initial_modules.push_back(module);
    for (const auto &[key, module] : modules) {
        bool has_failed_import = false;
        for (const auto &edge : import_graph) {
            if (edge.importer == key && !edge.error.empty() &&
                (edge.targetKind == ImportTargetKind::Zith ||
                 edge.targetKind == ImportTargetKind::Directory)) {
                has_failed_import = true;
                break;
            }
        }
        if (has_failed_import) {
            rebuilt.emplace_back(key, module);
            continue;
        }
        auto imported = importedMacrosFor(*module, initial_modules, import_graph);
        auto source   = module->source;
        rebuilt.emplace_back(key, buildModule(std::move(source), imported));
    }
    modules.clear();
    for (auto &[key, module] : rebuilt)
        modules.emplace(std::move(key), std::move(module));
    for (const auto &[key, module] : modules) {
        (void)key;
        for (const auto &diagnostic : module->diagnostics)
            module_diagnostics.push_back(diagnostic);
    }

    std::vector<ModuleArtifactPtr> ordered_modules;
    ordered_modules.reserve(modules.size());
    SnapshotMetrics snapshot_metrics;
    for (const auto &[key, module] : modules) {
        (void)key;
        ordered_modules.push_back(module);
        snapshot_metrics.artifactBytes +=
            module->source->text.size() +
            module->frontend->tokens().size() * sizeof(frontend::Token) +
            module->frontend->trivia().size() * sizeof(frontend::Trivia) +
            module->frontend->declarations().size() * sizeof(frontend::Declaration);
        snapshot_metrics.lexMs += module->timings.lexMs;
        snapshot_metrics.scanMs += module->timings.scanMs;
        snapshot_metrics.expandMs += module->timings.expandMs;
    }
    snapshot_metrics.moduleCount = ordered_modules.size();
    const auto cache_metrics     = cache_.metrics();
    snapshot_metrics.cacheHits   = cache_metrics.hits;
    snapshot_metrics.cacheMisses = cache_metrics.misses;

    std::sort(import_graph.begin(), import_graph.end(),
              [](const ImportEdge &left, const ImportEdge &right) {
                  if (left.importer != right.importer)
                      return left.importer < right.importer;
                  if (left.request.span.start != right.request.span.start)
                      return left.request.span.start < right.request.span.start;
                  return left.request.importKey() < right.request.importKey();
              });
    appendCycleDiagnostics(ordered_modules, resolved_dependencies, import_graph, diagnostics);
    auto resolutions = buildResolutions(ordered_modules, import_graph, diagnostics);
    for (auto &diagnostic : module_diagnostics)
        diagnostics.push_back(std::move(diagnostic));
    sortDiagnostics(diagnostics, *catalog_);
    auto merged_symbols = mergeSymbols(ordered_modules);
    std::vector<std::shared_ptr<const cinterop::CHeaderArtifact>> c_headers;
    {
        std::lock_guard<std::mutex> lock(c_headers_mutex_);
        c_headers.reserve(c_headers_by_path_.size());
        for (const auto item : c_headers_by_path_) {
            const auto &[path, header] = item;
            (void)path;
            c_headers.push_back(header);
        }
    }
    return std::make_shared<const CompilationSnapshot>(
        catalog_, cache_key_, root_key, std::move(ordered_modules), std::move(merged_symbols),
        std::move(import_graph), std::move(c_headers), std::move(resolutions),
        std::move(diagnostics), snapshot_metrics);
}

memory::Result<bool> FrontendContext::initializeStdlib() {
    std::lock_guard<std::mutex> lock(stdlib_mutex_);
    if (stdlib_initialized_)
        return true;

    for (const auto &root : config_.stdlibRoots) {
        for (const auto &path : collectDirectoryModules(root, -1)) {
            auto result = analyzeFile(path);
            if (!result)
                return std::move(result.error());
        }
    }
    stdlib_initialized_ = true;
    return true;
}

} // namespace zith::session
