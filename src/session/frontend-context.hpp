#pragma once

#include "cinterop/c-header.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/diagnostic.hpp"
#include "frontend/frontend.hpp"
#include "memory/result.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zith::session {

/// A stable, process-local content identity.  Two independent FNV streams make
/// accidental collisions impractical while keeping the key cheap to copy.
struct ContentFingerprint {
    uint64_t primary   = 0;
    uint64_t secondary = 0;

    constexpr bool operator==(const ContentFingerprint &) const noexcept = default;
    constexpr bool operator!=(const ContentFingerprint &) const noexcept = default;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] static ContentFingerprint fromText(std::string_view text) noexcept;
};

/// Immutable text registered by a workspace before a frontend worker sees it.
struct SourceRecord {
    memory::FileId id = 0;
    std::string canonicalPath;
    ContentFingerprint fingerprint;
    std::string text;
};

/// Append-only catalog shared by all frontend snapshots in a workspace.
///
/// `SourceMap` is intentionally not shared: it remains a mutable parser detail
/// inside each module artifact.  The catalog is the authoritative identity and
/// lifetime owner for source text exposed through snapshots.
class SourceCatalog {
public:
    using SourcePtr = std::shared_ptr<const SourceRecord>;

    SourceCatalog() = default;

    [[nodiscard]] static std::string canonicalPath(std::string_view path);
    [[nodiscard]] SourcePtr registerSource(std::string path, std::string text);
    [[nodiscard]] memory::Result<SourcePtr> loadFile(std::string_view path);
    [[nodiscard]] SourcePtr find(memory::FileId id) const;
    [[nodiscard]] SourcePtr find(std::string_view canonical_path,
                                 ContentFingerprint fingerprint) const;
    [[nodiscard]] size_t size() const noexcept;

private:
    struct SourceKey {
        std::string path;
        ContentFingerprint fingerprint;

        bool operator==(const SourceKey &) const noexcept = default;
    };

    struct SourceKeyHash {
        size_t operator()(const SourceKey &key) const noexcept;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<SourceKey, SourcePtr, SourceKeyHash> by_key_;
    std::vector<SourcePtr> by_id_;
};

/// Immutable fields which affect a cached frontend artifact.
struct CacheKey {
    std::string compilerVersion;
    std::string targetTriple;
    std::string parseFlags;
    std::string visibilityFlags;
    std::string sysroot;
    std::vector<std::string> includeRoots;
    std::vector<std::string> stdlibRoots;
    std::vector<std::string> cDefines;

    [[nodiscard]] std::string identity() const;
    bool operator==(const CacheKey &) const noexcept = default;
};

struct FrontendConfig {
    size_t maxFrontendWorkers = 1;
    std::string workspaceRoot;
    std::string compilerVersion;
    std::string targetTriple;
    std::string parseFlags;
    std::string visibilityFlags;
    std::string sysroot;
    std::vector<std::string> includeRoots;
    std::vector<std::string> stdlibRoots;
    std::vector<std::string> assetRoots;
    std::vector<std::string> cDefines;

    [[nodiscard]] CacheKey cacheKey() const;
};

/// Fixed-size executor used only for independent module frontend work.
/// It deliberately has no priority queue; LSP request scheduling belongs to a
/// higher layer and must remain independent from this module limit.
class ModuleExecutor {
public:
    explicit ModuleExecutor(size_t requested_workers = 1);
    ~ModuleExecutor();

    ModuleExecutor(const ModuleExecutor &)            = delete;
    ModuleExecutor &operator=(const ModuleExecutor &) = delete;

    [[nodiscard]] size_t workerCount() const noexcept {
        return worker_count_;
    }

    [[nodiscard]] static size_t normalizeWorkerCount(size_t requested) noexcept;

    template <typename Function>
    auto submit(Function &&function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task =
            std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();

#ifdef ZITH_IS_WASM
        (*task)();
#else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace_back([task]() { (*task)(); });
        }
        condition_.notify_one();
#endif
        return future;
    }

private:
#ifndef ZITH_IS_WASM
    void workerLoop();
#endif

    size_t worker_count_ = 1;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

using ModuleKey = std::string;

struct ImportSelectorRequest {
    std::string name;
    std::string alias;
    frontend::TextSpan span{};
    frontend::TextSpan aliasSpan{};
};

struct ImportRequest {
    std::vector<std::string> path;
    std::vector<frontend::TextSpan> pathSpans;
    std::vector<ImportSelectorRequest> selectors;
    std::string rawPath;
    std::string headerPath;
    std::string alias;
    bool isFrom   = false;
    bool isExport = false;
    bool isAsset  = false;
    bool isHeader = false;
    int32_t depth = 1;
    frontend::TextSpan span{};
    frontend::TextSpan pathSpan{};
    frontend::TextSpan aliasSpan{};

    [[nodiscard]] std::string importKey() const;
};

struct ModuleDiagnostic {
    diagnostics::Severity severity = diagnostics::Severity::Error;
    uint32_t code                  = 0;
    std::string message;
    memory::FileId file      = 0;
    memory::ByteOffset start = 0;
    memory::ByteOffset end   = 0;
};

struct LocalSymbolInfo {
    frontend::SymbolId id;
    std::string name;
    frontend::Visibility visibility = frontend::Visibility::Private;
    frontend::DeclKind kind         = frontend::DeclKind::Error;
    frontend::TextSpan span{};
};

struct ModuleTimings {
    double lexMs    = 0.0;
    double scanMs   = 0.0;
    double expandMs = 0.0;
};

struct ModuleArtifact {
    ModuleKey key;
    memory::FileId fileId = 0;
    ContentFingerprint fingerprint;
    SourceCatalog::SourcePtr source;
    std::shared_ptr<const frontend::FrontendSnapshot> frontend;
    std::vector<ImportRequest> imports;
    std::vector<LocalSymbolInfo> publicSymbols;
    std::vector<LocalSymbolInfo> moduleSymbols;
    std::vector<ModuleDiagnostic> diagnostics;
    ModuleTimings timings;

    [[nodiscard]] bool hasErrors() const noexcept;
};

using ModuleArtifactPtr = std::shared_ptr<const ModuleArtifact>;

struct ModuleSymbolRef {
    ModuleKey module;
    frontend::SymbolId localSymbol;
};

enum class ImportTargetKind : uint8_t { Zith, Directory, CHeader, CppHeader, Asset };

struct ImportEdge {
    ModuleKey importer;
    ImportRequest request;
    std::vector<ModuleKey> targets;
    ImportTargetKind targetKind = ImportTargetKind::Zith;
    std::shared_ptr<const cinterop::CHeaderArtifact> cHeader;
    std::string error;
};

enum class ResolutionKind : uint8_t { Declaration, Import, Foreign, ModuleAlias, Unresolved };

struct ResolvedName {
    std::string name;
    ResolutionKind kind = ResolutionKind::Unresolved;
    frontend::TextSpan span{};
    ModuleSymbolRef target;
    frontend::DeclId declaration;
    frontend::LocalId local;
    frontend::ScopeId scope;
    const cinterop::Function *foreignFunction = nullptr;
};

struct ModuleResolution {
    ModuleKey module;
    std::vector<ResolvedName> bindings;
    std::vector<ResolvedName> expressions;
};

struct MergedSymbol {
    std::string name;
    frontend::Visibility visibility = frontend::Visibility::Private;
    frontend::DeclKind kind         = frontend::DeclKind::Error;
    ModuleSymbolRef origin;
    frontend::TextSpan span{};
};

struct SnapshotMetrics {
    size_t moduleCount   = 0;
    size_t artifactBytes = 0;
    size_t cacheHits     = 0;
    size_t cacheMisses   = 0;
    double lexMs         = 0.0;
    double scanMs        = 0.0;
    double expandMs      = 0.0;
};

/// Immutable analysis view used by editor features.  All references held by a
/// snapshot stay valid after a cache entry is invalidated.
class CompilationSnapshot {
public:
    CompilationSnapshot(std::shared_ptr<const SourceCatalog> catalog, CacheKey cache_key,
                        std::vector<ModuleArtifactPtr> modules,
                        std::vector<MergedSymbol> merged_symbols,
                        std::vector<ImportEdge> import_graph,
                        std::vector<std::shared_ptr<const cinterop::CHeaderArtifact>> c_headers,
                        std::vector<ModuleResolution> resolutions,
                        std::vector<ModuleDiagnostic> diagnostics, SnapshotMetrics metrics);

    [[nodiscard]] const SourceCatalog &sourceCatalog() const noexcept {
        return *catalog_;
    }
    [[nodiscard]] const CacheKey &cacheKey() const noexcept {
        return cache_key_;
    }
    [[nodiscard]] const std::vector<ModuleArtifactPtr> &modules() const noexcept {
        return modules_;
    }
    [[nodiscard]] const std::vector<MergedSymbol> &mergedSymbols() const noexcept {
        return merged_symbols_;
    }
    [[nodiscard]] const std::vector<ImportEdge> &importGraph() const noexcept {
        return import_graph_;
    }
    [[nodiscard]] const std::vector<std::shared_ptr<const cinterop::CHeaderArtifact>> &
    cHeaders() const noexcept {
        return c_headers_;
    }
    [[nodiscard]] const std::vector<ModuleResolution> &resolutions() const noexcept {
        return resolutions_;
    }
    [[nodiscard]] const ModuleResolution *findResolution(std::string_view key) const noexcept;
    [[nodiscard]] const std::vector<ModuleDiagnostic> &diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] const SnapshotMetrics &metrics() const noexcept {
        return metrics_;
    }
    [[nodiscard]] const ModuleArtifact *findModule(std::string_view key) const noexcept;
    [[nodiscard]] bool hasErrors() const noexcept;

private:
    std::shared_ptr<const SourceCatalog> catalog_;
    CacheKey cache_key_;
    std::vector<ModuleArtifactPtr> modules_;
    std::vector<MergedSymbol> merged_symbols_;
    std::vector<ImportEdge> import_graph_;
    std::vector<std::shared_ptr<const cinterop::CHeaderArtifact>> c_headers_;
    std::vector<ModuleResolution> resolutions_;
    std::vector<ModuleDiagnostic> diagnostics_;
    SnapshotMetrics metrics_;
};

struct ModuleCacheMetrics {
    size_t hits        = 0;
    size_t misses      = 0;
    size_t invalidated = 0;
    size_t entries     = 0;
};

/// Thread-safe cache of the newest module artifact per canonical path and
/// CacheKey.  A snapshot owns old artifacts after replacement, not the cache.
class ModuleCache {
public:
    ModuleCache() = default;

    ModuleCache(const ModuleCache &)            = delete;
    ModuleCache &operator=(const ModuleCache &) = delete;

    [[nodiscard]] std::shared_future<ModuleArtifactPtr>
    getOrBuild(const CacheKey &cache_key, SourceCatalog::SourcePtr source, ModuleExecutor &executor,
               std::function<ModuleArtifactPtr()> build);

    /// Records which content revision is current for a path.  A new revision
    /// invalidates that module and all reverse dependents before it can publish.
    void noteSource(const SourceCatalog::SourcePtr &source);
    void updateDependencies(const ModuleKey &module, std::vector<ModuleKey> dependencies);
    void invalidate(std::string_view canonical_path);
    [[nodiscard]] ModuleCacheMetrics metrics() const;

private:
    struct InFlight {
        std::shared_future<ModuleArtifactPtr> future;
        uint64_t epoch = 0;
    };

    [[nodiscard]] static std::string bucketKey(const CacheKey &cache_key, std::string_view path);
    [[nodiscard]] static std::string inFlightKey(const CacheKey &cache_key,
                                                 const SourceRecord &source);
    [[nodiscard]] std::shared_future<ModuleArtifactPtr>
    readyFuture(ModuleArtifactPtr artifact) const;
    void invalidateLocked(const ModuleKey &module);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModuleArtifactPtr> artifacts_;
    std::unordered_map<std::string, InFlight> in_flight_;
    std::unordered_map<ModuleKey, ContentFingerprint> current_fingerprints_;
    std::unordered_map<ModuleKey, uint64_t> epochs_;
    std::unordered_map<ModuleKey, std::unordered_set<ModuleKey>> dependencies_;
    std::unordered_map<ModuleKey, std::unordered_set<ModuleKey>> reverse_dependencies_;
    ModuleCacheMetrics metrics_;
};

struct FrontendMetrics {
    ModuleCacheMetrics cache;
    size_t maxFrontendWorkers = 1;
};

/// Shared, workspace-scoped frontend state.  It has no global mutable parser
/// state and is suitable for use by several LSP request workers.
class FrontendContext {
public:
    explicit FrontendContext(FrontendConfig config = {});

    FrontendContext(const FrontendContext &)            = delete;
    FrontendContext &operator=(const FrontendContext &) = delete;

    [[nodiscard]] const FrontendConfig &config() const noexcept {
        return config_;
    }
    [[nodiscard]] const CacheKey &cacheKey() const noexcept {
        return cache_key_;
    }
    [[nodiscard]] size_t maxFrontendWorkers() const noexcept {
        return executor_.workerCount();
    }
    [[nodiscard]] FrontendMetrics metrics() const;

    [[nodiscard]] memory::Result<std::shared_ptr<const CompilationSnapshot>>
    analyzeFile(std::string_view path);
    [[nodiscard]] memory::Result<std::shared_ptr<const CompilationSnapshot>>
    analyzeText(std::string_view path, std::string text);

    /// Open-document content takes precedence over disk until removed.
    void setOverlay(std::string_view path, std::string text);
    void removeOverlay(std::string_view path);
    void invalidatePath(std::string_view path);

    /// Prebuild configured stdlib roots before accepting analysis requests.
    /// Callers such as an LSP initialize handler own the one-time policy.
    [[nodiscard]] memory::Result<bool> initializeStdlib();

    [[nodiscard]] std::shared_ptr<const SourceCatalog> sourceCatalog() const noexcept {
        return catalog_;
    }

private:
    struct PendingModule {
        ModuleKey key;
        SourceCatalog::SourcePtr source;
    };

    struct ResolvedImport {
        std::vector<ModuleKey> modules;
        ImportTargetKind targetKind = ImportTargetKind::Zith;
        bool found                  = false;
    };

    [[nodiscard]] memory::Result<SourceCatalog::SourcePtr> sourceForPath(std::string_view path);
    [[nodiscard]] memory::Result<std::shared_ptr<const CompilationSnapshot>>
    analyze(SourceCatalog::SourcePtr root_source);
    [[nodiscard]] ModuleArtifactPtr buildModule(SourceCatalog::SourcePtr source) const;
    [[nodiscard]] std::vector<std::string> visibleRootsFor(std::string_view root_path) const;
    [[nodiscard]] ResolvedImport resolveImport(const ModuleArtifact &artifact,
                                               const ImportRequest &request,
                                               const std::vector<std::string> &visible_roots) const;
    [[nodiscard]] static std::vector<std::string>
    collectDirectoryModules(std::string_view directory, int32_t depth);
    static void sortDiagnostics(std::vector<ModuleDiagnostic> &diagnostics,
                                const SourceCatalog &catalog);
    static std::vector<MergedSymbol> mergeSymbols(const std::vector<ModuleArtifactPtr> &modules);
    static std::vector<ModuleResolution>
    buildResolutions(const std::vector<ModuleArtifactPtr> &modules,
                     const std::vector<ImportEdge> &import_graph,
                     std::vector<ModuleDiagnostic> &diagnostics);
    static void
    appendCycleDiagnostics(const std::vector<ModuleArtifactPtr> &modules,
                           const std::map<ModuleKey, std::vector<ModuleKey>> &dependencies,
                           const std::vector<ImportEdge> &import_graph,
                           std::vector<ModuleDiagnostic> &diagnostics);

    FrontendConfig config_;
    CacheKey cache_key_;
    std::shared_ptr<SourceCatalog> catalog_;
    ModuleCache cache_;
    ModuleExecutor executor_;
    mutable std::shared_mutex overlay_mutex_;
    std::unordered_map<std::string, std::string> overlays_;
    std::mutex stdlib_mutex_;
    bool stdlib_initialized_ = false;
};

} // namespace zith::session
