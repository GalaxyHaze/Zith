#include "cache/cache-entry.hpp"

namespace toolkit::cache {

bool validateArtifact(const Artifact &artifact, std::uint32_t cache_key_hash,
                      const ContentFingerprint &fingerprint) {
    if (artifact.cache_key_hash != cache_key_hash)
        return false;
    const std::uint32_t hi = static_cast<std::uint32_t>(fingerprint.primary >> 32u);
    const std::uint32_t lo = static_cast<std::uint32_t>(fingerprint.primary & 0xFFFFFFFFu);
    if (artifact.source_fp_hi != hi || artifact.source_fp_lo != lo)
        return false;
    return true;
}

} // namespace toolkit::cache
