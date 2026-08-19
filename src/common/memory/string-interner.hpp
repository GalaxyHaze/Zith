#pragma once
#include "common/memory/arena.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/optional.hpp"
#include "support/macros.hpp"

#include <cstdint>
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

    void init();
};

} // namespace common::memory
