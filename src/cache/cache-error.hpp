#pragma once

#include "common/memory/result.hpp"

#include <cstdint>
#include <string>

namespace toolkit::cache {

enum class CacheErrorKind : std::uint8_t {
    Missing,
    Corrupt,
    UnsupportedVersion,
    ReadFailed,
    WriteFailed,
};

struct CacheError : common::memory::Error {
    CacheErrorKind kind = CacheErrorKind::Corrupt;

    CacheError(CacheErrorKind kind, std::string message)
        : common::memory::Error{std::move(message)}, kind(kind) {}
};

template <typename T>
using CacheResult = common::memory::Result<T, CacheError>;

} // namespace toolkit::cache
