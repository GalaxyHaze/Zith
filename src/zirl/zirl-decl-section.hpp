#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the Declarations section into `w`.
[[nodiscard]] bool encodeDecls(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the Declarations section from `r` into `out`.  Returns false on truncation.
[[nodiscard]] bool decodeDecls(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

namespace zith::zirl {

// Encodes the Declarations section into `w`.
[[nodiscard]] bool encodeDecls(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the Declarations section from `r` into `out`.  Returns false on truncation.
[[nodiscard]] bool decodeDecls(ByteReader &r, cache::Artifact &out);

// Encodes the Templates section into `w`.
[[nodiscard]] bool encodeTemplates(const cache::Artifact &artifact, ByteWriter &w);

// Decodes the Templates section from `r` into `out`.  Returns false on truncation.
[[nodiscard]] bool decodeTemplates(ByteReader &r, cache::Artifact &out);

} // namespace zith::zirl
