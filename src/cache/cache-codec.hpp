#pragma once

#include "cache/cache-error.hpp"
#include "cache/cache-types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace toolkit::cache {

[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes) noexcept;

// Serializes an artifact into the generic section-cache format. The returned
// bytes are deterministic for identical inputs. Section payloads are written
// by the generated codec (cache-codec.gen.*) from cache.rules.
[[nodiscard]] CacheResult<std::string> encodeArtifact(const Artifact &artifact);

// Validates and decodes a complete cache file. Validation order is magic,
// version/endianness, sizes/offsets, checksum, then section decoding.
[[nodiscard]] CacheResult<Artifact> decodeArtifact(std::string_view bytes);

} // namespace toolkit::cache
