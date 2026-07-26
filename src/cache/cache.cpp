#include "cache.hpp"

#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-writer.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace zith::cache {

namespace fs = std::filesystem;

namespace {

// Stable hash of a canonical path used to derive a flat artifact filename.
uint32_t pathHash(std::string_view path) {
    return zith::zirl::fnv1a32(path);
}

} // namespace

Store::Store(std::string cache_root, const session::CacheKey &cache_key)
    : root_(std::move(cache_root)), cache_key_hash_(zith::zirl::fnv1a32(cache_key.identity())),
      manifest_(root_ + "/modules") {
    std::error_code ec;
    fs::create_directories(root_ + "/modules", ec);
    manifest_.load();
}

std::string Store::artifactPath(std::string_view canonical_path) const {
    std::ostringstream oss;
    oss << root_ << "/modules/" << std::hex << pathHash(canonical_path) << ".zirl";
    return oss.str();
}

bool Store::validateArtifact(const Artifact &art,
                             const session::ContentFingerprint &fp) const {
    if (art.cache_key_hash != cache_key_hash_)
        return false;
    // Source fingerprint (64-bit primary) must match the current file.
    if (art.source_fp_hi != static_cast<uint32_t>(fp.primary >> 32u) ||
        art.source_fp_lo != static_cast<uint32_t>(fp.primary & 0xFFFFFFFFu))
        return false;
    return true;
}

std::optional<Artifact> Store::load(std::string_view canonical_path,
                                    const session::ContentFingerprint &fp) {
    const auto path = artifactPath(canonical_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        bumpMisses();
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto artifact = zith::zirl::Reader::read(bytes);
    if (!artifact) {
        bumpInvalid();
        return std::nullopt;
    }
    if (!validateArtifact(*artifact, fp)) {
        bumpInvalid();
        return std::nullopt;
    }
    // Validate dependencies against the manifest.
    for (const auto &dep : artifact->deps) {
        const auto dep_entry = manifest_.find(dep.canonical_path);
        if (!dep_entry) {
            bumpInvalid();
            return std::nullopt;
        }
        if (dep_entry->public_abi_hi != dep.public_abi_hi ||
            dep_entry->public_abi_lo != dep.public_abi_lo) {
            bumpInvalid();
            return std::nullopt;
        }
    }
    bumpHits();
    return artifact;
}

void Store::store(const Artifact &artifact) {
    const auto path = artifactPath(artifact.canonical_path);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    zith::zirl::ByteWriter writer;
    (void)zith::zirl::Writer::write(artifact, writer);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        bumpMisses();
        return;
    }
    out.write(reinterpret_cast<const char *>(writer.ptr()),
              static_cast<std::streamsize>(writer.size()));
    out.close();
    if (!out) {
        bumpMisses();
        return;
    }

    ManifestEntry entry;
    entry.canonical_path  = artifact.canonical_path;
    entry.artifact_path   = path;
    entry.public_abi_hi   = artifact.public_abi_hi;
    entry.public_abi_lo   = artifact.public_abi_lo;
    entry.source_fp_hi    = artifact.source_fp_hi;
    entry.source_fp_lo    = artifact.source_fp_lo;
    for (const auto &dep : artifact.deps) {
        entry.dependencies.push_back(dep.canonical_path);
    }
    manifest_.upsert(std::move(entry));
    manifest_.save();
    bumpWrites();
}

void Store::invalidate(std::string_view canonical_path) {
    const auto deps = manifest_.dependentsOf(canonical_path);
    // Remove the artifact file for each invalidated module.
    auto evict = [&](std::string_view p) {
        std::error_code ec;
        fs::remove(artifactPath(p), ec);
        manifest_.remove(p);
    };
    evict(canonical_path);
    for (const auto &dep : deps)
        evict(dep);
    manifest_.save();
    {
        std::lock_guard l(metrics_mutex_);
        metrics_.evictions += 1 + deps.size();
    }
}

StoreMetrics Store::metrics() const {
    std::lock_guard l(metrics_mutex_);
    return metrics_;
}

} // namespace zith::cache
