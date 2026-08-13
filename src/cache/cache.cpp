#include "cache/cache.hpp"

#include "cache/cache-codec.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace toolkit::cache {

namespace {

namespace fs = std::filesystem;

std::uint32_t pathHash(std::string_view path) {
    return static_cast<std::uint32_t>(fnv1a64(path));
}

} // namespace

Store::Store(std::string cache_root, const CacheKey &cache_key)
    : root_(std::move(cache_root)), cache_key_hash_(static_cast<std::uint32_t>(
                                        fnv1a64(cache_key.identity()))),
      manifest_(root_ + "/modules") {
    std::error_code ec;
    fs::create_directories(root_ + "/modules", ec);
    manifest_.load();
}

std::string Store::artifactPath(std::string_view canonical_path) const {
    std::ostringstream output;
    output << root_ << "/modules/" << std::hex << pathHash(canonical_path) << ".zgc";
    return output.str();
}

std::optional<Artifact> Store::load(std::string_view canonical_path,
                                    const ContentFingerprint &fingerprint) {
    const std::string path = artifactPath(canonical_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        bumpMisses();
        return std::nullopt;
    }
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    auto artifact = decodeArtifact(bytes);
    if (!artifact) {
        bumpInvalid();
        return std::nullopt;
    }
    if (!validateArtifact(artifact.value(), cache_key_hash_, fingerprint)) {
        bumpInvalid();
        return std::nullopt;
    }
    for (const auto &dep : artifact.value().deps) {
        const auto dep_entry = manifest_.find(dep.canonical_path);
        if (!dep_entry) {
            bumpInvalid();
            return std::nullopt;
        }
        if (dep.public_abi_hi == 0 && dep.public_abi_lo == 0)
            continue;
        if (dep_entry->public_abi_hi != dep.public_abi_hi ||
            dep_entry->public_abi_lo != dep.public_abi_lo) {
            bumpInvalid();
            return std::nullopt;
        }
    }
    bumpHits();
    return std::optional<Artifact>{std::in_place, std::move(artifact).value()};
}

std::optional<CacheEntry> Store::loadEntry(std::string_view canonical_path,
                                           const ContentFingerprint &fingerprint) {
    auto artifact = load(canonical_path, fingerprint);
    if (!artifact)
        return std::nullopt;
    CacheEntry entry;
    entry.artifact = std::move(*artifact);
    entry.fingerprint = fingerprint;
    entry.state = CacheEntryState::Hydrated;
    return std::optional<CacheEntry>{std::in_place, std::move(entry)};
}

void Store::store(const Artifact &artifact) {
    const std::string path = artifactPath(artifact.canonical_path);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    auto encoded = encodeArtifact(artifact);
    if (!encoded) {
        bumpMisses();
        return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        bumpMisses();
        return;
    }
    const std::string &bytes = encoded.value();
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        bumpMisses();
        return;
    }

    ManifestEntry entry;
    entry.canonical_path = artifact.canonical_path;
    entry.artifact_path = path;
    entry.public_abi_hi = artifact.public_abi_hi;
    entry.public_abi_lo = artifact.public_abi_lo;
    entry.source_fp_hi = artifact.source_fp_hi;
    entry.source_fp_lo = artifact.source_fp_lo;
    for (const auto &dep : artifact.deps)
        entry.dependencies.push_back(dep.canonical_path);
    manifest_.upsert(std::move(entry));
    manifest_.save();
    bumpWrites();
}

void Store::invalidate(std::string_view canonical_path) {
    const auto dependents = manifest_.dependentsOf(canonical_path);
    std::unordered_set<std::string> to_evict;
    to_evict.emplace(canonical_path);
    for (const auto &dependent : dependents)
        to_evict.emplace(dependent);
    for (const auto &path : to_evict) {
        std::error_code ec;
        fs::remove(artifactPath(path), ec);
        manifest_.remove(path);
    }
    manifest_.save();
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.evictions += to_evict.size();
    }
}

StoreMetrics Store::metrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
}

std::optional<ManifestEntry> Store::manifestEntry(std::string_view canonical_path) const {
    return manifest_.find(canonical_path);
}

} // namespace toolkit::cache
