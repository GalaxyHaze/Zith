#pragma once

#include "cache/cache-types.hpp"

#include <optional>
#include <string_view>

namespace zith::zirl {

// Decodes a zirl artifact.  Returns std::nullopt on any structural problem
// (truncation, bad magic, version mismatch, checksum failure).
class Reader {
public:
    [[nodiscard]] static std::optional<cache::Artifact> read(std::string_view bytes);
};

} // namespace zith::zirl
