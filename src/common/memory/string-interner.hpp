#pragma once
#include "common/memory/arena.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/optional.hpp"
#include "support/macros.hpp"

#include <cstdint>
#if !defined(ZCT_IS_WASM)
#include <shared_mutex>
#endif
#include <string_view>

#include "common/memory/optional.hpp"

namespace common::memory {

using InternedId = uint32_t;

class Arena;
template <class T> class DynArray;

struct StringInterner {
    aSelf(StringInterner);

    explicit StringInterner(Arena &arena);
    StringInterner(const Self &)          = delete;
    auto operator=(const Self &) -> Self & = delete;

    InternedId intern(std::string_view str);
    std::string_view lookup(InternedId id) const;
    Optional<InternedId> findId(std::string_view str) const;

private:
    Arena *allocator_                        = nullptr;
    FlatMap<std::string_view, InternedId> *map = nullptr;
    DynArray<std::string_view> *pool   = nullptr;
#if !defined(ZCT_IS_WASM)
    mutable std::shared_mutex rwMutex_;
#endif

    void init();
};

} // namespace common::memory
