#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the Code section (lowered HIR function bodies) into `w`.
[[nodiscard]] bool encodeCode(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the Code section from `r` into `out`.  Returns false on truncation.
[[nodiscard]] bool decodeCode(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
