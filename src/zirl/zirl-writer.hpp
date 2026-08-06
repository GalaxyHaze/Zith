#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"

#include <cstdint>

namespace zith::zirl {

// Serializes a cache::Artifact into a byte-stable zirl file.
class Writer {
public:
    [[nodiscard]] static uint32_t write(const cache::Artifact &artifact, ByteWriter &out);
};

} // namespace zith::zirl
