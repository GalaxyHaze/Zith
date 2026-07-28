#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zith::cache {

// Per-module manifest entry kept in memory and persisted to
// `<cacheRoot>/manifest`.  Tracks the public ABI hash of each cached module
// and the set of modules that import it, so a dependency change can transitively
// invalidate dependents.
struct ManifestEntry {
    std::string canonical_path;
    std::string artifact_path;
    uint32_t public_abi_hi = 0;
    uint32_t public_abi_lo = 0;
    uint32_t source_fp_hi  = 0;
    uint32_t source_fp_lo  = 0;
    std::vector<std::string> dependencies; // modules this one imports
};

class Manifest {
    std::string root_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ManifestEntry> by_path_;
    std::unordered_map<std::string, std::unordered_set<std::string>> reverse_deps_;

public:
    explicit Manifest(std::string root) : root_(std::move(root)) {}

    void upsert(ManifestEntry entry);
    void remove(std::string_view canonical_path);
    [[nodiscard]] std::optional<ManifestEntry> find(std::string_view canonical_path) const;

    // Return modules that directly or transitively import `canonical_path`.
    [[nodiscard]] std::vector<std::string> dependentsOf(std::string_view canonical_path) const;

    // Persist to and load from `<root_>/manifest`.  Best-effort: load returns an
    // empty manifest on any error rather than failing compilation.
    void save() const;
    void load();

    [[nodiscard]] const std::string &root() const noexcept {
        return root_;
    }
};

} // namespace zith::cache
