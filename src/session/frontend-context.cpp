#include "session/frontend-context.hpp"

#include "diagnostics/error-codes.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace zith::session {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kFnvOffset    = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime     = 1099511628211ULL;
constexpr uint64_t kSecondOffset = 7809847782465536322ULL;
constexpr uint64_t kSecondPrime  = 14029467366897019727ULL;

struct FnvParameters {
    uint64_t offset;
    uint64_t prime;
};

[[nodiscard]] uint64_t hashText(std::string_view text, FnvParameters parameters) noexcept {
    uint64_t hash = parameters.offset;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= parameters.prime;
    }
    return hash;
}

[[nodiscard]] std::string joinPath(const std::vector<std::string> &path) {
    std::string result;
    for (size_t index = 0; index < path.size(); ++index) {
        if (index != 0U)
            result += '/';
        result += path[index];
    }
    return result;
}

[[nodiscard]] std::string stableRoot(std::string_view path) {
    if (path.empty())
        return {};
    return SourceCatalog::canonicalPath(path);
}

void normalizeRoots(std::vector<std::string> &roots) {
    std::vector<std::string> normalized;
    normalized.reserve(roots.size());
    for (const auto &root : roots) {
        auto canonical = stableRoot(root);
        if (canonical.empty() ||
            std::find(normalized.begin(), normalized.end(), canonical) != normalized.end())
            continue;
        normalized.push_back(std::move(canonical));
    }
    roots = std::move(normalized);
}

[[nodiscard]] bool isZithFile(const fs::path &path) {
    return path.extension() == ".zith";
}

[[nodiscard]] bool isHeaderFile(const fs::path &path) {
    const auto extension = path.extension();
    return extension == ".h" || extension == ".hpp";
}

[[nodiscard]] ModuleDiagnostic makeImportDiagnostic(const ModuleArtifact &artifact,
                                                    const ImportRequest &request,
                                                    std::string message) {
    return {
        diagnostics::Severity::Error, diagnostics::err::ImportError,
        std::move(message),           artifact.fileId,
        request.span.start,           request.span.end,
    };
}

} // namespace

std::string ContentFingerprint::toString() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << primary << std::setw(16)
           << secondary;
    return output.str();
}

ContentFingerprint ContentFingerprint::fromText(const std::string_view text) noexcept {
    return {
        hashText(text, {kFnvOffset, kFnvPrime}),
        hashText(text, {kSecondOffset, kSecondPrime}),
    };
}

std::string SourceCatalog::canonicalPath(const std::string_view path) {
#ifdef ZITH_IS_WASM
    return std::string(path);
#else
    std::error_code error;
    auto absolute = fs::absolute(fs::path(path), error);
    if (error)
        return fs::path(path).lexically_normal().generic_string();

    auto canonical = fs::weakly_canonical(absolute, error);
    if (error)
        return absolute.lexically_normal().generic_string();
    return canonical.generic_string();
#endif
}

size_t SourceCatalog::SourceKeyHash::operator()(const SourceKey &key) const noexcept {
    const auto path_hash = std::hash<std::string>{}(key.path);
    const auto first     = std::hash<uint64_t>{}(key.fingerprint.primary);
    const auto second    = std::hash<uint64_t>{}(key.fingerprint.secondary);
    return path_hash ^ (first << 1U) ^ (second << 7U);
}

SourceCatalog::SourcePtr SourceCatalog::registerSource(std::string path, std::string text) {
    path                   = canonicalPath(path);
    const auto fingerprint = ContentFingerprint::fromText(text);
    SourceKey key{path, fingerprint};

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (const auto existing = by_key_.find(key); existing != by_key_.end())
        return existing->second;

    if (by_id_.size() >= static_cast<size_t>(std::numeric_limits<memory::FileId>::max()))
        return {};

    const auto id = static_cast<memory::FileId>(by_id_.size());
    auto source   = std::make_shared<const SourceRecord>(
        SourceRecord{id, std::move(path), fingerprint, std::move(text)});
    by_key_.emplace(std::move(key), source);
    by_id_.push_back(source);
    return source;
}

memory::Result<SourceCatalog::SourcePtr> SourceCatalog::loadFile(const std::string_view path) {
#ifdef ZITH_IS_WASM
    (void)path;
    return memory::Error{"loading source files is not available on WASM"};
#else
    const auto canonical = canonicalPath(path);
    std::ifstream input(canonical, std::ios::binary);
    if (!input)
        return memory::Error{"failed to load '" + canonical + "'"};

    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return registerSource(canonical, std::move(content));
#endif
}

SourceCatalog::SourcePtr SourceCatalog::find(const memory::FileId id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (id >= by_id_.size())
        return {};
    return by_id_[id];
}

SourceCatalog::SourcePtr SourceCatalog::find(const std::string_view canonical_path,
                                             const ContentFingerprint fingerprint) const {
    const SourceKey key{canonicalPath(canonical_path), fingerprint};
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (const auto existing = by_key_.find(key); existing != by_key_.end())
        return existing->second;
    return {};
}

size_t SourceCatalog::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return by_id_.size();
}

std::string CacheKey::identity() const {
    std::ostringstream output;
    // The published module shape changed from legacy parser objects to frontend artifacts.
    output << "frontend-artifact-v2\n"
           << compilerVersion << '\n'
           << targetTriple << '\n'
           << parseFlags << '\n'
           << visibilityFlags << '\n';
    for (const auto &root : includeRoots)
        output << "I:" << root << '\n';
    for (const auto &root : stdlibRoots)
        output << "S:" << root << '\n';
    return output.str();
}

CacheKey FrontendConfig::cacheKey() const {
    CacheKey key{
        compilerVersion, targetTriple, parseFlags, visibilityFlags, includeRoots, stdlibRoots,
    };
    normalizeRoots(key.includeRoots);
    normalizeRoots(key.stdlibRoots);
    return key;
}

size_t ModuleExecutor::normalizeWorkerCount(const size_t requested) noexcept {
#ifdef ZITH_IS_WASM
    (void)requested;
    return 1;
#else
    const auto hardware = std::thread::hardware_concurrency();
    const auto maximum  = hardware == 0U ? size_t{1} : static_cast<size_t>(hardware);
    const auto minimum  = requested == 0U ? size_t{1} : requested;
    return std::min(minimum, maximum);
#endif
}

ModuleExecutor::ModuleExecutor(const size_t requested_workers)
    : worker_count_(normalizeWorkerCount(requested_workers)) {
#ifndef ZITH_IS_WASM
    workers_.reserve(worker_count_);
    for (size_t worker = 0; worker < worker_count_; ++worker)
        workers_.emplace_back([this]() { workerLoop(); });
#endif
}

ModuleExecutor::~ModuleExecutor() {
#ifndef ZITH_IS_WASM
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto &worker : workers_)
        worker.join();
#endif
}

#ifndef ZITH_IS_WASM
void ModuleExecutor::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}
#endif

std::string ImportRequest::importKey() const {
    return joinPath(path);
}

bool ModuleArtifact::hasErrors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const ModuleDiagnostic &diagnostic) {
                           return diagnostic.severity == diagnostics::Severity::Error ||
                                  diagnostic.severity == diagnostics::Severity::Bug;
                       });
}

CompilationSnapshot::CompilationSnapshot(std::shared_ptr<const SourceCatalog> catalog,
                                         CacheKey cache_key, std::vector<ModuleArtifactPtr> modules,
                                         std::vector<MergedSymbol> merged_symbols,
                                         std::vector<ModuleDiagnostic> diagnostics,
                                         SnapshotMetrics metrics)
    : catalog_(std::move(catalog)), cache_key_(std::move(cache_key)), modules_(std::move(modules)),
      merged_symbols_(std::move(merged_symbols)), diagnostics_(std::move(diagnostics)),
      metrics_(metrics) {}

const ModuleArtifact *CompilationSnapshot::findModule(const std::string_view key) const noexcept {
    const auto found = std::lower_bound(
        modules_.begin(), modules_.end(), key,
        [](const ModuleArtifactPtr &module, std::string_view name) { return module->key < name; });
    if (found == modules_.end() || (*found)->key != key)
        return nullptr;
    return found->get();
}

bool CompilationSnapshot::hasErrors() const noexcept {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const ModuleDiagnostic &diagnostic) {
                           return diagnostic.severity == diagnostics::Severity::Error ||
                                  diagnostic.severity == diagnostics::Severity::Bug;
                       });
}

std::string ModuleCache::bucketKey(const CacheKey &cache_key, const std::string_view path) {
    return cache_key.identity() + "\x1f" + std::string(path);
}

std::string ModuleCache::inFlightKey(const CacheKey &cache_key, const SourceRecord &source) {
    return bucketKey(cache_key, source.canonicalPath) + "\x1e" + source.fingerprint.toString();
}

std::shared_future<ModuleArtifactPtr> ModuleCache::readyFuture(ModuleArtifactPtr artifact) const {
    std::promise<ModuleArtifactPtr> promise;
    auto future = promise.get_future().share();
    promise.set_value(std::move(artifact));
    return future;
}

std::shared_future<ModuleArtifactPtr>
ModuleCache::getOrBuild(const CacheKey &cache_key, SourceCatalog::SourcePtr source,
                        ModuleExecutor &executor, std::function<ModuleArtifactPtr()> build) {
    const auto bucket    = bucketKey(cache_key, source->canonicalPath);
    const auto in_flight = inFlightKey(cache_key, *source);
    uint64_t epoch       = 0;
    std::shared_ptr<std::promise<ModuleArtifactPtr>> promise;
    std::shared_future<ModuleArtifactPtr> shared;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto current = current_fingerprints_.find(source->canonicalPath);
        if (current == current_fingerprints_.end() || current->second != source->fingerprint) {
            current_fingerprints_[source->canonicalPath] = source->fingerprint;
            ++epochs_[source->canonicalPath];
            invalidateLocked(source->canonicalPath);
        }
        if (const auto cached = artifacts_.find(bucket);
            cached != artifacts_.end() && cached->second->fingerprint == source->fingerprint) {
            ++metrics_.hits;
            return readyFuture(cached->second);
        }
        if (const auto active = in_flight_.find(in_flight); active != in_flight_.end()) {
            ++metrics_.hits;
            return active->second.future;
        }
        ++metrics_.misses;
        epoch   = epochs_[source->canonicalPath];
        promise = std::make_shared<std::promise<ModuleArtifactPtr>>();
        shared  = promise->get_future().share();
        in_flight_.emplace(in_flight, InFlight{shared, epoch});
    }

    (void)executor.submit([this, bucket, in_flight, worker_source = source, epoch,
                           worker_promise = std::move(promise),
                           worker_build   = std::move(build)]() mutable {
        auto artifact = worker_build();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto active = in_flight_.find(in_flight);
            if (active != in_flight_.end())
                in_flight_.erase(active);

            if (artifact && epochs_[worker_source->canonicalPath] == epoch &&
                current_fingerprints_[worker_source->canonicalPath] == worker_source->fingerprint) {
                artifacts_[bucket] = artifact;
                metrics_.entries   = artifacts_.size();
            }
        }
        worker_promise->set_value(std::move(artifact));
    });
    return std::shared_future<ModuleArtifactPtr>{shared};
}

void ModuleCache::noteSource(const SourceCatalog::SourcePtr &source) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = current_fingerprints_.find(source->canonicalPath);
    if (current != current_fingerprints_.end() && current->second == source->fingerprint)
        return;

    current_fingerprints_[source->canonicalPath] = source->fingerprint;
    ++epochs_[source->canonicalPath];
    invalidateLocked(source->canonicalPath);
}

void ModuleCache::updateDependencies(const ModuleKey &module, std::vector<ModuleKey> dependencies) {
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto previous = dependencies_.find(module); previous != dependencies_.end()) {
        for (const auto &dependency : previous->second)
            reverse_dependencies_[dependency].erase(module);
    }

    auto &stored = dependencies_[module];
    stored.clear();
    for (auto &dependency : dependencies) {
        stored.insert(dependency);
        reverse_dependencies_[dependency].insert(module);
    }
}

void ModuleCache::invalidate(const std::string_view canonical_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epochs_[std::string(canonical_path)];
    invalidateLocked(std::string(canonical_path));
}

void ModuleCache::invalidateLocked(const ModuleKey &module) {
    std::vector<ModuleKey> pending{module};
    std::unordered_set<ModuleKey> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;

        for (auto iterator = artifacts_.begin(); iterator != artifacts_.end();) {
            if (iterator->second->key == current) {
                iterator = artifacts_.erase(iterator);
                ++metrics_.invalidated;
            } else {
                ++iterator;
            }
        }
        if (const auto dependents = reverse_dependencies_.find(current);
            dependents != reverse_dependencies_.end()) {
            for (const auto &dependent : dependents->second) {
                ++epochs_[dependent];
                pending.push_back(dependent);
            }
        }
    }
    metrics_.entries = artifacts_.size();
}

ModuleCacheMetrics ModuleCache::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

FrontendContext::FrontendContext(FrontendConfig config)
    : config_(std::move(config)), cache_key_(), catalog_(std::make_shared<SourceCatalog>()),
      cache_(), executor_(config_.maxFrontendWorkers) {
    config_.workspaceRoot = stableRoot(config_.workspaceRoot);
    normalizeRoots(config_.includeRoots);
    normalizeRoots(config_.stdlibRoots);
    cache_key_ = config_.cacheKey();
}

FrontendMetrics FrontendContext::metrics() const {
    return {cache_.metrics(), executor_.workerCount()};
}

memory::Result<SourceCatalog::SourcePtr>
FrontendContext::sourceForPath(const std::string_view path) {
    const auto canonical = SourceCatalog::canonicalPath(path);
    {
        std::shared_lock<std::shared_mutex> lock(overlay_mutex_);
        if (const auto overlay = overlays_.find(canonical); overlay != overlays_.end())
            return catalog_->registerSource(canonical, overlay->second);
    }
    return catalog_->loadFile(canonical);
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

void FrontendContext::invalidatePath(const std::string_view path) {
    cache_.invalidate(SourceCatalog::canonicalPath(path));
}

std::vector<std::string> FrontendContext::visibleRootsFor(const std::string_view root_path) const {
    (void)root_path;
    std::vector<std::string> roots;
    roots.reserve(config_.stdlibRoots.size() + config_.includeRoots.size() + 2U);
    roots.insert(roots.end(), config_.stdlibRoots.begin(), config_.stdlibRoots.end());
    roots.insert(roots.end(), config_.includeRoots.begin(), config_.includeRoots.end());
    if (!config_.workspaceRoot.empty())
        roots.push_back(config_.workspaceRoot);
#ifndef ZITH_IS_WASM
    roots.push_back(fs::path(root_path).parent_path().generic_string());
#endif
    normalizeRoots(roots);
    return roots;
}

std::vector<std::string> FrontendContext::collectDirectoryModules(const std::string_view directory,
                                                                  const int32_t depth) {
    std::vector<std::string> paths;
#ifndef ZITH_IS_WASM
    std::error_code error;
    const fs::path base(directory);
    if (!fs::is_directory(base, error))
        return paths;

    const auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator iterator(base, options, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (depth != -1) {
            const auto relative = iterator.depth() + 1;
            if (relative > depth) {
                iterator.disable_recursion_pending();
                continue;
            }
        }
        if (iterator->is_regular_file(error) && isZithFile(iterator->path()))
            paths.push_back(SourceCatalog::canonicalPath(iterator->path().generic_string()));
    }
#else
    (void)directory;
    (void)depth;
#endif
    std::sort(paths.begin(), paths.end());
    return paths;
}

FrontendContext::ResolvedImport
FrontendContext::resolveImport(const ModuleArtifact &artifact, const ImportRequest &request,
                               const std::vector<std::string> &visible_roots) const {
    if (request.isAsset)
        return {{}, true};

#ifdef ZITH_IS_WASM
    (void)artifact;
    (void)request;
    (void)visible_roots;
    return {};
#else
    std::vector<fs::path> candidates;
    const fs::path import_path(request.importKey());
    candidates.push_back(fs::path(artifact.key).parent_path() / import_path);
    for (const auto &root : visible_roots)
        candidates.push_back(fs::path(root) / import_path);

    std::optional<fs::path> imported;
    for (const auto &candidate : candidates) {
        std::error_code error;
        if (fs::exists(candidate, error)) {
            imported = candidate;
            break;
        }
        const auto zith_file = candidate.string() + ".zith";
        if (fs::exists(zith_file, error)) {
            imported = fs::path(zith_file);
            break;
        }
        const auto module_file = candidate / "mod.zith";
        if (fs::exists(module_file, error)) {
            imported = module_file;
            break;
        }
    }
    if (!imported)
        return {};
    const fs::path resolved(*imported);
    if (isHeaderFile(resolved))
        return {{}, true};
    if (fs::is_directory(resolved))
        return {collectDirectoryModules(resolved.generic_string(), request.depth), true};
    if (!isZithFile(resolved))
        return {{}, true};
    return {{SourceCatalog::canonicalPath(resolved.generic_string())}, true};
#endif
}

ModuleArtifactPtr FrontendContext::buildModule(SourceCatalog::SourcePtr source) const {
    auto artifact         = std::make_shared<ModuleArtifact>();
    artifact->key         = source->canonicalPath;
    artifact->fileId      = source->id;
    artifact->fingerprint = source->fingerprint;
    artifact->source      = source;

    const auto parse_start = std::chrono::steady_clock::now();
    artifact->frontend =
        std::make_shared<const frontend::FrontendSnapshot>(frontend::parse(source->text));
    artifact->timings.lexMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start)
            .count();

    for (const auto &diagnostic : artifact->frontend->diagnostics()) {
        artifact->diagnostics.push_back({
            diagnostics::Severity::Error,
            diagnostics::err::UnknownToken,
            diagnostic.message,
            source->id,
            diagnostic.span.start,
            diagnostic.span.end,
        });
    }
    for (const auto &declaration : artifact->frontend->declarations()) {
        if (declaration.kind == frontend::DeclKind::Import) {
            artifact->imports.push_back({declaration.import.path, declaration.import.isFrom,
                                         declaration.import.isExport, declaration.import.isAsset,
                                         declaration.import.depth, declaration.span});
            continue;
        }
        if (declaration.kind == frontend::DeclKind::Error ||
            declaration.visibility == frontend::Visibility::Private)
            continue;
        LocalSymbolInfo symbol{frontend::SymbolId{declaration.id.value}, declaration.name,
                               declaration.visibility, declaration.kind, declaration.span};
        if (symbol.visibility == frontend::Visibility::Public)
            artifact->publicSymbols.push_back(std::move(symbol));
        else
            artifact->moduleSymbols.push_back(std::move(symbol));
    }
    std::sort(artifact->imports.begin(), artifact->imports.end(),
              [](const ImportRequest &left, const ImportRequest &right) {
                  return left.importKey() < right.importKey();
              });
    return artifact;
}

std::vector<MergedSymbol>
FrontendContext::mergeSymbols(const std::vector<ModuleArtifactPtr> &modules) {
    std::vector<MergedSymbol> result;
    for (const auto &module : modules) {
        auto append = [&](const LocalSymbolInfo &symbol) {
            result.push_back({
                symbol.name,
                symbol.visibility,
                symbol.kind,
                ModuleSymbolRef{module->key, symbol.id},
                symbol.span,
            });
        };
        for (const auto &symbol : module->publicSymbols)
            append(symbol);
        for (const auto &symbol : module->moduleSymbols)
            append(symbol);
    }
    std::sort(result.begin(), result.end(),
              [](const MergedSymbol &left, const MergedSymbol &right) {
                  if (left.name != right.name)
                      return left.name < right.name;
                  if (left.origin.module != right.origin.module)
                      return left.origin.module < right.origin.module;
                  return left.origin.localSymbol.value < right.origin.localSymbol.value;
              });
    return result;
}

void FrontendContext::appendCycleDiagnostics(
    const std::vector<ModuleArtifactPtr> &modules,
    const std::map<ModuleKey, std::vector<ModuleKey>> &dependencies,
    std::vector<ModuleDiagnostic> &diagnostics) {
    std::unordered_map<ModuleKey, const ModuleArtifact *> by_key;
    std::unordered_map<ModuleKey, std::vector<ModuleKey>> edges;
    for (const auto &module : modules)
        by_key.emplace(module->key, module.get());
    for (const auto &[module, module_dependencies] : dependencies)
        edges.emplace(module, module_dependencies);

    enum class Mark : uint8_t { None, Active, Done };
    std::unordered_map<ModuleKey, Mark> marks;
    std::vector<ModuleKey> stack;
    std::unordered_set<std::string> reported;
    std::function<void(const ModuleKey &)> visit = [&](const ModuleKey &key) {
        marks[key] = Mark::Active;
        stack.push_back(key);
        for (const auto &next : edges[key]) {
            if (marks[next] == Mark::None) {
                visit(next);
                continue;
            }
            if (marks[next] != Mark::Active)
                continue;

            const auto begin = std::find(stack.begin(), stack.end(), next);
            std::vector<ModuleKey> cycle(begin, stack.end());
            cycle.push_back(next);
            std::ostringstream message;
            message << "circular import detected: ";
            for (size_t index = 0; index < cycle.size(); ++index) {
                if (index != 0U)
                    message << " -> ";
                message << cycle[index];
            }
            if (!reported.insert(message.str()).second)
                continue;
            const auto *module = by_key[key];
            diagnostics.push_back({diagnostics::Severity::Error, diagnostics::err::ImportError,
                                   message.str(), module->fileId, 0, 0});
        }
        stack.pop_back();
        marks[key] = Mark::Done;
    };
    for (const auto &module : modules)
        if (marks[module->key] == Mark::None)
            visit(module->key);
}

void FrontendContext::sortDiagnostics(std::vector<ModuleDiagnostic> &diagnostics,
                                      const SourceCatalog &catalog) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [&catalog](const ModuleDiagnostic &left, const ModuleDiagnostic &right) {
                  const auto left_source  = catalog.find(left.file);
                  const auto right_source = catalog.find(right.file);
                  const auto left_path = left_source ? left_source->canonicalPath : std::string{};
                  const auto right_path =
                      right_source ? right_source->canonicalPath : std::string{};
                  if (left_path != right_path)
                      return left_path < right_path;
                  if (left.start != right.start)
                      return left.start < right.start;
                  if (left.code != right.code)
                      return left.code < right.code;
                  return left.message < right.message;
              });
}

memory::Result<std::shared_ptr<const CompilationSnapshot>>
FrontendContext::analyze(SourceCatalog::SourcePtr root_source) {
    const auto visible_roots = visibleRootsFor(root_source->canonicalPath);
    std::map<ModuleKey, SourceCatalog::SourcePtr> pending;
    std::map<ModuleKey, ModuleArtifactPtr> modules;
    std::map<ModuleKey, std::vector<ModuleKey>> resolved_dependencies;
    std::vector<ModuleDiagnostic> diagnostics;
    pending.emplace(root_source->canonicalPath, std::move(root_source));

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
            for (const auto &module_diagnostic : module->diagnostics)
                diagnostics.push_back(module_diagnostic);

            std::vector<ModuleKey> dependencies;
            for (const auto &request : module->imports) {
                const auto resolved = resolveImport(*module, request, visible_roots);
                if (!resolved.found) {
                    diagnostics.push_back(makeImportDiagnostic(*module, request,
                                                               "could not resolve import '" +
                                                                   request.importKey() + "'"));
                    continue;
                }
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

    appendCycleDiagnostics(ordered_modules, resolved_dependencies, diagnostics);
    sortDiagnostics(diagnostics, *catalog_);
    auto merged_symbols = mergeSymbols(ordered_modules);
    return std::make_shared<const CompilationSnapshot>(
        catalog_, cache_key_, std::move(ordered_modules), std::move(merged_symbols),
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
