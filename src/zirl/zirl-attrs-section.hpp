#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the residual HIR attribute section (slot ownership, call escapes,
// function-level facts) into `w`.
[[nodiscard]] bool encodeAttrs(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the HIR attribute section from `r` into `out`.
[[nodiscard]] bool decodeAttrs(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
