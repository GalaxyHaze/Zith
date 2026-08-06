#pragma once

#include "cache/cache-types.hpp"
#include "session/frontend-context.hpp"

#include <cstdint>

namespace zith::cache {

enum class CacheEntryState : uint8_t {
    Invalid,
    Hydrating,
    Hydrated,
};

// A validated, in-memory cache entry produced by Store::loadEntry.
// Holds the decoded artifact together with the source fingerprint that was
// validated, plus a hydration-state marker used by CompilationSession to
// coordinate short-circuit stages.
struct CacheEntry {
    Artifact artifact;
    session::ContentFingerprint fingerprint;
    CacheEntryState state = CacheEntryState::Invalid;
};

// Validate an artifact against the session cache key and source fingerprint.
// Extracted from Store so CacheEntry consumers can re-validate without
// reaching into a Store instance.
[[nodiscard]] bool validateArtifact(const Artifact &art, uint32_t cache_key_hash,
                                    const session::ContentFingerprint &fp);

} // namespace zith::cache
