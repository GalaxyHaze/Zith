#pragma once

#include <type_traits>
#include <utility>

#define moveVar(var) std::move(other.var)
#define moveInit(var) var(moveVar(var))
#define moveExchange(var) std::exchange(other.var, nullptr)
#define aSelf(type) using Self = type
#define allocaPtr(var) var = alloc<decltype(var)>(allocator)
#define initializeAt(var, ...) std::construct_at(&var, __VA_ARGS__)

template <class T> constexpr T alloc(auto &&allocator) {
    static_assert(std::is_pointer_v<T>, "Type must be a Pointer");
    using U = std::remove_cvref_t<decltype(allocator)>;
    if constexpr (std::is_pointer_v<U>)
        return (T)allocator->alloc(sizeof(void *));
    return (T)allocator.alloc(sizeof(void *));
}
