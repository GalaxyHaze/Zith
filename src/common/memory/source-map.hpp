#pragma once
#include "common/memory/arena.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/optional.hpp"
#include "common/memory/result.hpp"
#include "common/memory/source-file.hpp"
#include "common/memory/span.hpp"

#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace common::memory {

struct SourceSpan {
    FileId file = 0;
    Span span{};
};

class SourceMap {
    Arena file_arena;
    DynArray<SourceLoc> files;
    FlatMap<std::string, FileId> cache;
    mutable std::shared_mutex rw_mutex;

public:
    SourceMap();

    [[nodiscard]] auto addFile(std::string_view path, std::string_view content)
        -> Result<FileId>;

    [[nodiscard]] auto loadFile(std::string_view path, bool write = false)
        -> Result<FileId>;

    [[nodiscard]] auto exists(FileId id) const noexcept -> bool;

    [[nodiscard]] auto snippet(const SourceSpan &span) const noexcept -> Result<std::string_view>;

    [[nodiscard]] auto get(FileId id) const noexcept -> Optional<std::reference_wrapper<const SourceLoc>>;
    [[nodiscard]] auto view(FileId id) const noexcept -> Optional<std::string_view>;

    [[nodiscard]] auto loc(const SourceSpan &span) const noexcept -> Loc;
};

} // namespace common::memory
