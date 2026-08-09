#pragma once
#include "common/arena.hpp"
#include "common/flat-map.hpp"
#include "common/optional.hpp"
#include "common/result.hpp"
#include "common/source-file.hpp"
#include "common/span.hpp"

#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace zith::memory {

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

    [[nodiscard]] auto snippet(const Span &span) noexcept -> Result<std::string_view>;

    [[nodiscard]] auto get(FileId id) noexcept -> Optional<std::reference_wrapper<SourceLoc>>;

    [[nodiscard]] auto loc(const Span &span) const noexcept -> Loc;
};

} // namespace zith::memory
