#include "common/memory/source-file.hpp"

#include <algorithm>
#include <string_view>

namespace common::memory {

auto SourceLoc::slice() const noexcept -> std::string_view {
    return std::visit([](const auto &stored) -> std::string_view {
        if constexpr (requires { stored.slice(); })
            return stored.slice();
        else
            return {stored.data(), stored.size()};
    }, file);
}

auto SourceLoc::data() const noexcept -> const char * {
    return std::visit([](const auto &stored) -> const char * {
        if constexpr (requires { stored.data(); })
            return stored.data();
        else
            return stored.c_str();
    }, file);
}

void SourceLoc::buildLines() noexcept {
    line_starts.clear();
    line_starts.push(0);
    const std::string_view content = slice();
    for (ByteOffset i = 0; i < content.size(); ++i) {
        if (content[i] == '\n')
            line_starts.push(i + 1);
    }
}

auto SourceLoc::loc(ByteOffset offset) const noexcept -> Loc {
    const std::string_view content = slice();
    if (offset >= content.size())
        return Loc{};
    if (line_starts.empty())
        return Loc{1, static_cast<ByteOffset>(offset + 1)};

    const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    const size_t line_index = static_cast<size_t>(std::distance(line_starts.begin(), it) - 1);
    return Loc{
        static_cast<ByteOffset>(line_index + 1),
        static_cast<ByteOffset>(offset - line_starts[line_index] + 1),
    };
}

auto SourceLoc::snippet(const Span &span) const noexcept -> Result<std::string_view> {
    const std::string_view content = slice();
    if (span.start > content.size() || span.end > content.size() || span.start > span.end)
        return Error{"Span out of file bounds"};
    return content.substr(span.start, span.end - span.start);
}

auto SourceLoc::filename() const noexcept -> std::string_view {
    const size_t last_slash = path.find_last_of('/');
    if (last_slash == std::string::npos)
        return path;
    return std::string_view(path).substr(last_slash + 1);
}

} // namespace common::memory
