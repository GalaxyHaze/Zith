#pragma once

#include "cache/cache-types.hpp"

#include <cstdint>

namespace toolkit::cache {

enum class CacheEntryState : std::uint8_t {
    Invalid,
    Hydrating,
    Hydrated,
};

struct CacheEntry {
    Artifact artifact;
    ContentFingerprint fingerprint;
    CacheEntryState state = CacheEntryState::Invalid;
};

[[nodiscard]] bool validateArtifact(const Artifact &artifact, std::uint32_t cache_key_hash,
                                    const ContentFingerprint &fingerprint);

} // namespace toolkit::cache
