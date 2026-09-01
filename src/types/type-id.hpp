#pragma once

#include <cstdint>

namespace zith::types {

using TypeId = uint32_t;

inline constexpr TypeId kErrorType   = 0;
inline constexpr TypeId kNeverType   = 1;
inline constexpr TypeId kVoidType    = 2;
inline constexpr TypeId kBoolType    = 3;
inline constexpr TypeId kCharType    = 4;
inline constexpr TypeId kFirstCustom = 5;
inline constexpr TypeId kInvalidType = ~TypeId{0};

/// Stable 128-bit canonical identity used to hydrate opaque tags and to
/// expose `at-canonicalType(T)`.  Runtime opaque tags stay project-local u32;
/// this value is the compiler-side type identity.
struct TypeCanonicalId {
    uint64_t hi = 0;
    uint64_t lo = 0;

    constexpr bool operator==(const TypeCanonicalId &) const noexcept = default;
    constexpr bool operator!=(const TypeCanonicalId &) const noexcept = default;
};

inline constexpr TypeCanonicalId kInvalidCanonicalId{};

} // namespace zith::types
