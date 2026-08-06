#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the Metadata section (strings, paths, compact types) into `w`.
[[nodiscard]] bool encodeTypes(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the Metadata section from `r` into `out`.  Returns false on truncation.
[[nodiscard]] bool decodeTypes(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
