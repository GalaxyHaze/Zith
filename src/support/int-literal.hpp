#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace zith::support {

/// Outcome of parsing an integer literal as the lexer produced it.
enum class IntLiteralStatus : std::uint8_t {
    Ok,
    /// The text is not an integer literal at all (float, string, identifier, ...).
    NotInteger,
    /// The digits form a valid literal whose magnitude does not fit 64 bits.
    Overflow,
};

/// Integer type suffixes are accepted by the lexer as part of a literal token and
/// only affect the literal's type, never its bit pattern.
[[nodiscard]] constexpr std::string_view integerSuffix(std::string_view text) noexcept {
    constexpr std::string_view kSuffixes[] = {
        "u128", "i128", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8", "usize", "isize",
    };
    for (const auto suffix : kSuffixes) {
        if (text.size() >= suffix.size() && text.ends_with(suffix))
            return suffix;
    }
    return {};
}

namespace detail {

/// Digit value for `c` in `base`, or -1 when `c` is not a digit of that base.
[[nodiscard]] constexpr int digitValue(char c, unsigned base) noexcept {
    int value = -1;
    if (c >= '0' && c <= '9')
        value = c - '0';
    else if (c >= 'a' && c <= 'f')
        value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
        value = c - 'A' + 10;
    return (value >= 0 && value < static_cast<int>(base)) ? value : -1;
}

/// An optional sign plus an optional radix prefix split off the literal text. The
/// prefixes match the lexer exactly: `0x`/`0X` hex, `0c`/`0C` octal, `0b`/`0B` binary.
struct IntLiteralShape {
    bool negative = false;
    unsigned base = 10U;
    std::string_view digits;
};

[[nodiscard]] constexpr bool shapeOf(std::string_view text, IntLiteralShape &shape) noexcept {
    if (text.empty())
        return false;
    std::size_t i = 0;
    if (text[0] == '-' || text[0] == '+') {
        shape.negative = text[0] == '-';
        i++;
    }
    if (text.size() - i >= 2U && text[i] == '0') {
        const char prefix = text[i + 1U];
        if (prefix == 'x' || prefix == 'X')
            shape.base = 16U;
        else if (prefix == 'c' || prefix == 'C')
            shape.base = 8U;
        else if (prefix == 'b' || prefix == 'B')
            shape.base = 2U;
        if (shape.base != 10U)
            i += 2U;
    }
    shape.digits      = text.substr(i);
    const auto suffix = integerSuffix(shape.digits);
    if (!suffix.empty())
        shape.digits.remove_suffix(suffix.size());
    if (shape.digits.empty())
        return false;
    for (char c : shape.digits) {
        if (digitValue(c, shape.base) < 0)
            return false;
    }
    return true;
}

} // namespace detail

/// True when `text` is an integer literal in any base the lexer accepts.
[[nodiscard]] constexpr bool looksIntegerLiteral(std::string_view text) noexcept {
    detail::IntLiteralShape shape;
    return detail::shapeOf(text, shape);
}

/// Parses an integer literal honouring its radix prefix. An unsigned magnitude above
/// `INT64_MAX` is kept as its two's-complement bit pattern so masks such as
/// `0xFFFFFFFFFFFFFFFF` survive; anything wider than 64 bits is reported as Overflow
/// instead of being silently truncated.
[[nodiscard]] constexpr IntLiteralStatus parseIntegerLiteral(std::string_view text,
                                                             std::int64_t &out) noexcept {
    detail::IntLiteralShape shape;
    if (!detail::shapeOf(text, shape))
        return IntLiteralStatus::NotInteger;

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t magnitude      = 0;
    for (char c : shape.digits) {
        const auto digit = static_cast<std::uint64_t>(detail::digitValue(c, shape.base));
        if (magnitude > (kMax - digit) / shape.base)
            return IntLiteralStatus::Overflow;
        magnitude = magnitude * shape.base + digit;
    }

    if (shape.negative) {
        constexpr std::uint64_t kMinMagnitude = std::uint64_t{1} << 63U;
        if (magnitude > kMinMagnitude)
            return IntLiteralStatus::Overflow;
        out = magnitude == kMinMagnitude ? std::numeric_limits<std::int64_t>::min()
                                         : -static_cast<std::int64_t>(magnitude);
    } else {
        out = static_cast<std::int64_t>(magnitude);
    }
    return IntLiteralStatus::Ok;
}

} // namespace zith::support
