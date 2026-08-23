#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the monomorphized-instance summary into `w`.
[[nodiscard]] bool encodeInstantiations(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the monomorphized-instance summary from `r` into `out`.
[[nodiscard]] bool decodeInstantiations(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
