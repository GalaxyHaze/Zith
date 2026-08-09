#pragma once
#include "common/arena.hpp"
#include "common/flat-map.hpp"
#include "support/macros.hpp"

#include <cstdint>
#if !defined(ZITH_IS_WASM)
#include <shared_mutex>
#endif
#include <string_view>

namespace zith::memory {

using InternedId = uint32_t;

class Arena;
template <class T> class DynArray;

struct StringInterner {
    aSelf(StringInterner);

    StringInterner() = default;
    explicit StringInterner(memory::Arena &arena);
    StringInterner(const Self &)          = delete;
    auto operator=(const Self &) -> Self & = delete;

    InternedId intern(std::string_view str);
    std::string_view lookup(InternedId id) const;

private:
    Arena *allocator_                        = nullptr;
    FlatMap<std::string_view, InternedId> *map = nullptr;
    memory::DynArray<std::string_view> *pool   = nullptr;
#if !defined(ZITH_IS_WASM)
    mutable std::shared_mutex rwMutex_;
#endif

    void init();
};

} // namespace zith::memory
