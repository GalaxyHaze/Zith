#include "cache/cache-entry.hpp"

namespace zith::cache {

bool validateArtifact(const Artifact &art, uint32_t cache_key_hash,
                      const session::ContentFingerprint &fp) {
    if (art.cache_key_hash != cache_key_hash)
        return false;
    if (art.source_fp_hi != static_cast<uint32_t>(fp.primary >> 32u) ||
        art.source_fp_lo != static_cast<uint32_t>(fp.primary & 0xFFFFFFFFu))
        return false;
    return true;
}

} // namespace zith::cache
