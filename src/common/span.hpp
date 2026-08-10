#pragma once
#include "common/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace memory {

using ByteOffset = uint32_t;
using FileId     = uint32_t;

struct Loc {
    ByteOffset line = 1;
    ByteOffset col  = 1;

    auto toString() const {
        return std::to_string(line) + ":" + std::to_string(col);
    }
};

struct Span {
    FileId file;
    ByteOffset start;
    ByteOffset end;

    auto len() const noexcept -> size_t {
        return end >= start ? static_cast<size_t>(end - start) : 0;
    }

    bool contains(ByteOffset offset) const noexcept {
        return offset >= start && offset < end;
    }

    bool isValid() const noexcept {
        return end >= start;
    }

    auto merge(const Span &other) const noexcept -> Result<Span> {
        if (file != other.file)
            return Result<Span>(Error{"Cannot merge spans from different files"});

        const bool overlaps = (start < other.end) && (other.start < end);
        if (!overlaps)
            return Result<Span>(Error{"Spans do not overlap and cannot be merged"});

        return Span{file, std::min(start, other.start), std::max(end, other.end)};
    }
};

} // namespace memory
