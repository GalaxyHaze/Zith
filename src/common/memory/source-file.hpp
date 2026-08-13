#pragma once
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/file-source.hpp"
#include "common/memory/result.hpp"
#include "common/memory/span.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace common::memory {

struct SourceLoc {
    using FileVar = std::variant<FileSource, std::string>;

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

} // namespace common::memory
