#pragma once

#include "cache/cache-entry.hpp"
#include "cache/cache-types.hpp"
#include "cache/manifest.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace toolkit::cache {

struct StoreMetrics {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t invalid = 0;
    std::size_t writes = 0;
    std::size_t evictions = 0;
};

class Store {
public:
    Store(std::string cache_root, const CacheKey &cache_key);

    [[nodiscard]] std::optional<Artifact> load(std::string_view canonical_path,
                                               const ContentFingerprint &fingerprint);
    [[nodiscard]] std::optional<CacheEntry> loadEntry(std::string_view canonical_path,
                                                      const ContentFingerprint &fingerprint);
    void store(const Artifact &artifact);
    void invalidate(std::string_view canonical_path);
    [[nodiscard]] StoreMetrics metrics() const;
    [[nodiscard]] std::optional<ManifestEntry> manifestEntry(
        std::string_view canonical_path) const;
    [[nodiscard]] const std::string &root() const noexcept {
        return root_;
    }

private:
    std::string root_;
    std::uint32_t cache_key_hash_ = 0;
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

} // namespace toolkit::cache
