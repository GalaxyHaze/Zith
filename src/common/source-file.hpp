#pragma once
#include "common/arena.hpp"
#include "common/dyn-array.hpp"
#include "common/result.hpp"
#include "common/span.hpp"

#include <mio/mmap.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace zith::memory {

struct SourceLoc {
    using FileVar = std::variant<mio::mmap_source, mio::mmap_sink, std::string>;

    FileVar file;
    std::string path;
    DynArray<ByteOffset> line_starts;

    SourceLoc(FileVar file, std::string path, Arena &arena)
        : file(std::move(file)), path(std::move(path)), line_starts(arena) {}

    [[nodiscard]] auto slice() const noexcept -> std::string_view;
    [[nodiscard]] auto data() const noexcept -> const char *;

    void buildLines() noexcept;
    [[nodiscard]] auto loc(ByteOffset offset) const noexcept -> Loc;

    [[nodiscard]] auto snippet(const Span &span) const noexcept -> Result<std::string_view>;
    [[nodiscard]] auto filename() const noexcept -> std::string_view;
};

} // namespace zith::memory
