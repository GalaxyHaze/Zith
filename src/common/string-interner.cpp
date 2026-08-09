#include "common/string-interner.hpp"

#include "common/arena.hpp"
#include "common/dyn-array.hpp"
#include "common/flat-map.hpp"
#include "support/macros.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#if !defined(ZITH_IS_WASM)
#include <mutex>
#include <shared_mutex>
#endif

namespace zith::memory {

StringInterner::StringInterner(Arena &arena) : allocator_(&arena) {
    init();
}

InternedId StringInterner::intern(std::string_view str) {
#if !defined(ZITH_IS_WASM)
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
#endif

    auto *existing = map->get(str);
    if (existing)
        return *existing;

    auto *copy = static_cast<char *>(allocator_->alloc(str.size(), 1));
    std::memcpy(copy, str.data(), str.length());
    const InternedId id = static_cast<InternedId>(pool->size());
    pool->emplace(copy, str.length());
    map->insert((*pool)[id], id);
    return id;
}

std::string_view StringInterner::lookup(InternedId id) const {
#if !defined(ZITH_IS_WASM)
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
#endif

    if (id >= pool->size())
        return {};
    return (*pool)[id];
}

void StringInterner::init() {
    pool = allocator_->make<DynArray<std::string_view>>(*allocator_);
    map  = allocator_->make<FlatMap<std::string_view, InternedId>>();
    if (!pool || !map) {
        std::fprintf(
            stderr,
            "at: StringInterner, occured an internal error, due to: nullptr at allocator\n");
        std::abort();
    }
}

} // namespace zith::memory
