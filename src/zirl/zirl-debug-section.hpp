#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Reserved debug section.  Encodes an empty payload so the section is valid
// in the section table; decoders that encounter a non-empty payload ignore it
// without failing.
[[nodiscard]] bool encodeDebug(const cache::Artifact &artifact, ByteWriter &w);
[[nodiscard]] bool decodeDebug(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
