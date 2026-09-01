#include "cache.hpp"

#include "cache/cache-entry.hpp"
#include "memory/flat-set.hpp"
#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-writer.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace zith::cache {

namespace fs = std::filesystem;

namespace {

// Stable hash of a canonical path used to derive a flat artifact filename.
uint32_t pathHash(std::string_view path) {
    return zith::zirl::fnv1a32(path);
}

bool parseHexU64(std::string_view text, uint64_t &out) {
    const auto *begin = text.data();
    const auto *end   = begin + text.size();
    auto [ptr, ec]    = std::from_chars(begin, end, out, 16);
    return ec == std::errc{} && ptr == end;
}

bool parseDecimalU32(std::string_view text, uint32_t &out) {
    const auto *begin = text.data();
    const auto *end   = begin + text.size();
    auto [ptr, ec]    = std::from_chars(begin, end, out, 10);
    return ec == std::errc{} && ptr == end;
}

} // namespace

Store::Store(std::string cache_root, const session::CacheKey &cache_key)
    : root_(std::move(cache_root)), cache_key_hash_(zith::zirl::fnv1a32(cache_key.identity())),
      manifest_(root_ + "/modules") {
    std::error_code ec;
    fs::create_directories(root_ + "/modules", ec);
    manifest_.load();
    loadCanonicalRegistry();
}

uint32_t Store::assignCanonicalId(const types::TypeCanonicalId &canonical_id) {
    std::lock_guard<std::mutex> lock(canonical_mutex_);
    if (const auto existing = canonical_registry_.find(canonical_id);
        existing != canonical_registry_.end())
        return existing->second;
    const uint32_t tag = static_cast<uint32_t>(canonical_registry_.size()) + 1U;
    if (tag == 0U || canonical_by_tag_.contains(tag))
        return 0U;
    canonical_registry_[canonical_id] = tag;
    canonical_by_tag_[tag]            = canonical_id;
    saveCanonicalRegistryLocked();
    return tag;
}

std::string Store::artifactPath(std::string_view canonical_path) const {
    std::ostringstream oss;
    oss << root_ << "/modules/" << std::hex << pathHash(canonical_path) << ".zirl";
    return oss.str();
}

std::string Store::canonicalRegistryPath() const {
    return root_ + "/canonical-any";
}

void Store::loadCanonicalRegistry() {
    std::lock_guard<std::mutex> lock(canonical_mutex_);
    canonical_registry_.clear();
    canonical_by_tag_.clear();
    const auto path = fs::path(canonicalRegistryPath());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;
        const auto first = line.find(':');
        const auto last  = line.rfind(':');
        if (first == std::string::npos || last == std::string::npos || first == last)
            continue;
        types::TypeCanonicalId canonical_id;
        uint32_t tag = 0;
        if (!parseHexU64(std::string_view(line).substr(0, first), canonical_id.hi) ||
            !parseHexU64(std::string_view(line).substr(first + 1, last - first - 1),
                         canonical_id.lo) ||
            !parseDecimalU32(std::string_view(line).substr(last + 1), tag) || tag == 0U)
            continue;
        if (canonical_registry_.contains(canonical_id) || canonical_by_tag_.contains(tag))
            continue;
        canonical_registry_[canonical_id] = tag;
        canonical_by_tag_[tag]            = canonical_id;
    }
}

void Store::saveCanonicalRegistryLocked() {
    std::error_code ec;
    fs::create_directories(root_, ec);
    const auto path = fs::path(canonicalRegistryPath());
    const auto tmp  = fs::path(canonicalRegistryPath() + ".tmp");
    std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
    if (!output)
        return;
    for (const auto &[canonical_id, tag] : canonical_registry_) {
        output << std::hex << std::nouppercase << canonical_id.hi << ':' << canonical_id.lo << ':'
               << std::dec << tag << '\n';
    }
    output.close();
    if (!output)
        return;
    fs::rename(tmp, path, ec);
}

void Store::dropInvalid(std::string_view canonical_path) {
    std::error_code ec;
    fs::remove(artifactPath(canonical_path), ec);
    manifest_.remove(canonical_path);
    bumpInvalid();
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
        dropInvalid(canonical_path);
        return std::nullopt;
    }
    if (!validateArtifact(*artifact, cache_key_hash_, fp)) {
        dropInvalid(canonical_path);
        return std::nullopt;
    }
    // Validate dependencies against the manifest.
    for (const auto &dep : artifact->deps) {
        const auto dep_entry = manifest_.find(dep.canonical_path);
        if (!dep_entry) {
            dropInvalid(canonical_path);
            return std::nullopt;
        }
        if (dep.public_abi_hi == 0 && dep.public_abi_lo == 0)
            continue;
        if (dep_entry->public_abi_hi != dep.public_abi_hi ||
            dep_entry->public_abi_lo != dep.public_abi_lo) {
            dropInvalid(canonical_path);
            return std::nullopt;
        }
    }
    bumpHits();
    return std::optional<Artifact>{std::in_place, std::move(*artifact)};
}

std::optional<CacheEntry> Store::loadEntry(std::string_view canonical_path,
                                           const session::ContentFingerprint &fp) {
    auto art = load(canonical_path, fp);
    if (!art)
        return std::nullopt;
    CacheEntry entry;
    entry.artifact    = std::move(*art);
    entry.fingerprint = fp;
    entry.state       = CacheEntryState::Hydrated;
    return std::optional<CacheEntry>{std::in_place, std::move(entry)};
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
    entry.canonical_path = artifact.canonical_path;
    entry.artifact_path  = path;
    entry.public_abi_hi  = artifact.public_abi_hi;
    entry.public_abi_lo  = artifact.public_abi_lo;
    entry.source_fp_hi   = artifact.source_fp_hi;
    entry.source_fp_lo   = artifact.source_fp_lo;
    for (const auto &dep : artifact.deps) {
        entry.dependencies.push_back(dep.canonical_path);
    }
    manifest_.upsert(std::move(entry));
    manifest_.save();
    bumpWrites();
}

void Store::invalidate(std::string_view canonical_path) {
    const auto deps = manifest_.dependentsOf(canonical_path);
    // Collect all paths to evict: original + transitive dependents, deduplicated.
    // Deduplication guards against any overlap between dependentsOf output and
    // the input path, and ensures idempotency over dependency cycles.
    memory::FlatSet<std::string> to_evict;
    to_evict.insert(canonical_path);
    for (const auto &dep : deps)
        to_evict.insert(dep);
    for (const auto &p : to_evict) {
        std::error_code ec;
        fs::remove(artifactPath(p), ec);
        manifest_.remove(p);
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

} // namespace zith::cache
