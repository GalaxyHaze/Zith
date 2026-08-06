#pragma once

#include "cache/cache-entry.hpp"
#include "cache/cache-types.hpp"
#include "cache/manifest.hpp"
#include "session/frontend-context.hpp" // session::CacheKey, session::ContentFingerprint
#include "zirl/zirl-header.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zith::cache {

struct StoreMetrics {
    size_t hits      = 0;
    size_t misses    = 0;
    size_t invalid   = 0;
    size_t writes    = 0;
    size_t evictions = 0;
};

// Persistent per-module artifact store.  Owns the on-disk cache directory and a
// Manifest for reverse-dependency invalidation.  Thread-safe.
class Store {
public:
    Store(std::string cache_root, const session::CacheKey &cache_key);

    // Try to load and validate the artifact for `canonical_path` whose source
    // fingerprint is `fp`.  Returns the artifact on a full hit, or std::nullopt
    // on any miss/invalidation/corruption.  Increments metrics accordingly.
    [[nodiscard]] std::optional<Artifact> load(std::string_view canonical_path,
                                               const session::ContentFingerprint &fp);

    // Try to load and validate the artifact for `canonical_path` whose source
    // fingerprint is `fp`.  Returns a fully validated CacheEntry on a full hit,
    // or std::nullopt on any miss/invalidation/corruption.
    [[nodiscard]] std::optional<CacheEntry> loadEntry(std::string_view canonical_path,
                                                      const session::ContentFingerprint &fp);

    // Persist `artifact` to disk and update the manifest.  Best-effort: disk
    // write failures are swallowed and only reflected in metrics.
    void store(const Artifact &artifact);

    // Invalidate a module and all of its transitive dependents.
    void invalidate(std::string_view canonical_path);

    [[nodiscard]] StoreMetrics metrics() const;
    [[nodiscard]] std::optional<ManifestEntry> manifestEntry(std::string_view canonical_path) const;

    [[nodiscard]] const std::string &root() const noexcept {
        return root_;
    }

private:
    std::string root_;
    uint32_t cache_key_hash_ = 0;
    Manifest manifest_;
    mutable std::mutex metrics_mutex_;
    StoreMetrics metrics_;

    [[nodiscard]] std::string artifactPath(std::string_view canonical_path) const;
    void bumpHits() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.hits;
    }
    void bumpMisses() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.misses;
    }
    void bumpInvalid() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.invalid;
    }
    void bumpWrites() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.writes;
    }
};

} // namespace zith::cache
