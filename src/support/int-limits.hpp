#pragma once

#include <cstddef>
#include <cstdint>

namespace zith::support {

inline constexpr std::uint64_t kMaxU32 = 0xFFFFFFFFULL;

/// True when `size` fits in a 32-bit unsigned field.
///
/// The comparison is widened to `uint64_t` so it stays meaningful, and does not
/// trip `-Wtautological-type-limit-compare`, on targets where `std::size_t` is
/// itself only 32 bits wide, such as WebAssembly.
[[nodiscard]] constexpr bool fitsInU32(std::size_t size) noexcept {
    return static_cast<std::uint64_t>(size) <= kMaxU32;
}

/// True when `size` does not fit in a 32-bit unsigned field. See `fitsInU32`.
[[nodiscard]] constexpr bool exceedsU32(std::size_t size) noexcept {
    return static_cast<std::uint64_t>(size) > kMaxU32;
}

} // namespace zith::support
