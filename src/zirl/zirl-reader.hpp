#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-header.hpp"

#include <optional>
#include <string>

namespace zith::zirl {

// Decodes a zirl artifact.  Returns std::nullopt on any structural problem
// (truncation, bad magic, version mismatch, checksum failure).  Callers must
// treat a failed read as a corrupted artifact and fall back to source.
class Reader {
public:
    [[nodiscard]] static std::optional<cache::Artifact> read(std::string_view bytes);

private:
    static bool readMetadata(ByteReader &r, cache::Artifact &out);
    static bool readDecls(ByteReader &r, cache::Artifact &out);
    static bool readTemplates(ByteReader &r, cache::Artifact &out);
    static bool readCode(ByteReader &r, cache::Artifact &out);
    static bool readCompactType(ByteReader &r, cache::CompactType &out);
    static bool readCompactExpr(ByteReader &r, cache::CompactExpr &out);
};

} // namespace zith::zirl
