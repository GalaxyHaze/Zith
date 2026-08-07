#pragma once

#include "hir/hir-expr.hpp"
#include "types/type-kind.hpp"

#include <string_view>

namespace zith::sema {

[[nodiscard]] hir::HirBinaryOp mapBinaryOp(std::string_view text) noexcept;
[[nodiscard]] hir::HirUnaryOp mapUnaryOp(std::string_view text) noexcept;

[[nodiscard]] bool isComparisonOp(std::string_view text) noexcept;
[[nodiscard]] bool isArithmeticOp(std::string_view text) noexcept;
[[nodiscard]] bool isShiftOp(std::string_view text) noexcept;
[[nodiscard]] bool isBitwiseOp(std::string_view text) noexcept;

/// Maps a sema integer width to the interned HIR width.
[[nodiscard]] types::IntWidth mapIntegerWidth(uint8_t bits, bool is_signed) noexcept;
/// Maps a sema float width to the interned HIR width.
[[nodiscard]] types::FloatWidth mapFloatWidth(uint8_t bits) noexcept;

} // namespace zith::sema
