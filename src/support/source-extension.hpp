#pragma once

#include <filesystem>
#include <string_view>

namespace zith::support {

/// Canonical source extension for the Zith-- frontend.
///
/// Zith-- owns the explicit `.zithm` extension (the `m` marks the minus form).
/// The shared `.zith` extension is reserved for the flagship Zith language, so
/// the current compiler reads only `.zithm` sources and leaves `.zith` free.
inline constexpr std::string_view kZithmSourceExtension = ".zithm";

/// True when `path` carries the Zith-- source extension.
[[nodiscard]] inline bool isZithmSource(const std::filesystem::path &path) {
    return path.extension() == kZithmSourceExtension;
}

/// True when `text` ends with the Zith-- source extension.
[[nodiscard]] constexpr bool hasZithmSourceExtension(std::string_view text) noexcept {
    return text.size() >= kZithmSourceExtension.size() &&
           text.substr(text.size() - kZithmSourceExtension.size()) == kZithmSourceExtension;
}

/// Removes the trailing Zith-- source extension from `text`, if present.
[[nodiscard]] constexpr std::string_view stripZithmSourceExtension(std::string_view text) noexcept {
    return hasZithmSourceExtension(text)
               ? text.substr(0, text.size() - kZithmSourceExtension.size())
               : text;
}

} // namespace zith::support
