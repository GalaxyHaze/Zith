#pragma once

#include "cache/cache-types.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-header.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::zirl {

// Serializes a cache::Artifact into a byte-stable zirl file.  The writer is
// deterministic: identical semantic input always produces identical bytes.
class Writer {
public:
    // Encode `artifact` into `out`.  Returns the computed file checksum.
    [[nodiscard]] static uint32_t write(const cache::Artifact &artifact, ByteWriter &out);

private:
    static void writeMetadata(const cache::Artifact &artifact, ByteWriter &w);
    static void writeDecls(const cache::Artifact &artifact, ByteWriter &w);
    static void writeTemplates(const cache::Artifact &artifact, ByteWriter &w);
    static void writeCode(const cache::Artifact &artifact, ByteWriter &w);
    static void writeCompactType(const cache::CompactType &t, ByteWriter &w);
    static void writeCompactExpr(const cache::CompactExpr &e, ByteWriter &w);
};

} // namespace zith::zirl
