// impl/memory/arena.hpp — Memory arena management (C++ wrapper)
//
// Centralizes arena utilities and provides RAII helpers.
// The C implementation remains in arena_c_functions.c — this header
// provides C++-friendly access patterns and the ArenaList template.
#pragma once

#include <cstddef>
#include <cstring>
#include <zith/zith.hpp>

#ifdef __cplusplus

namespace zith {

// ArenaList<T> — Chunked list backed by arena allocation
// Originally from impl/memory/utils.h — moved here for centralization.

template<typename T>
class ArenaList {
    struct Chunk {
        Chunk *next;
        size_t len;
        size_t capacity;

        T *items() { return reinterpret_cast<T *>(this + 1); }
        const T *items() const { return reinterpret_cast<const T *>(this + 1); }
    };

    Chunk *head_ = nullptr;
    Chunk *tail_ = nullptr;
    size_t total_ = 0;
    size_t chunk_cap_ = 0;

public:
    ArenaList() = default;

    // Initialize or reset the list for reuse
    void init(ZithArena *arena, size_t chunk_capacity = 16) {
        head_ = nullptr;
        tail_ = nullptr;
        total_ = 0;
        chunk_cap_ = chunk_capacity > 0 ? chunk_capacity : 16;
    }

    [[nodiscard]] size_t size() const { return total_; }
    [[nodiscard]] bool empty() const { return total_ == 0; }

    // Append an element — allocates a new chunk if needed
    void push(ZithArena *arena, const T &value) {
        if (!tail_ || tail_->len == tail_->capacity) {
            alloc_chunk(arena);
        }
        if (!tail_) return; // allocation failed
        tail_->items()[tail_->len++] = value;
        total_++;
    }

    // Flatten all chunks into a contiguous arena-backed array
    T *flatten(ZithArena *arena, size_t *out_count) const {
        *out_count = total_;
        if (total_ == 0) return nullptr;

        T *arr = static_cast<T *>(zith_arena_alloc(arena, total_ * sizeof(T)));
        if (!arr) { *out_count = 0; return nullptr; }

        size_t i = 0;
        for (const Chunk *c = head_; c; c = c->next) {
            std::memcpy(arr + i, c->items(), c->len * sizeof(T));
            i += c->len;
        }
        return arr;
    }

    // Random access — O(n/chunk_capacity), for debug only
    T *at(size_t index) {
        for (Chunk *c = head_; c; c = c->next) {
            if (index < c->len) return &c->items()[index];
            index -= c->len;
        }
        return nullptr;
    }

    const T *at(size_t index) const {
        for (const Chunk *c = head_; c; c = c->next) {
            if (index < c->len) return &c->items()[index];
            index -= c->len;
        }
        return nullptr;
    }

    // Range-for iteration
    struct Iterator {
        const Chunk *chunk;
        size_t index;

        bool operator!=(const Iterator &o) const {
            return chunk != o.chunk || index != o.index;
        }
        Iterator &operator++() {
            if (!chunk) return *this;
            if (++index >= chunk->len) { chunk = chunk->next; index = 0; }
            while (chunk && chunk->len == 0) chunk = chunk->next;
            return *this;
        }
        const T &operator*() const { return chunk->items()[index]; }
    };

    Iterator begin() const {
        const Chunk *c = head_;
        while (c && c->len == 0) c = c->next;
        return {c, 0};
    }
    Iterator end() const { return {nullptr, 0}; }

private:
    void alloc_chunk(ZithArena *arena) {
        const size_t sz = sizeof(Chunk) + chunk_cap_ * sizeof(T);
        auto *c = static_cast<Chunk *>(zith_arena_alloc(arena, sz));
        if (!c) return;
        c->next = nullptr; c->len = 0; c->capacity = chunk_cap_;
        if (tail_) tail_->next = c; else head_ = c;
        tail_ = c;
    }
};

} // namespace zith

#endif // __cplusplus

// ============================================================================
// C-compatible ArenaList — for code that doesn't use C++
// This is the original impl/memory/utils.h content, kept for compatibility
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// C users should use the template above via zith::ArenaList<T>
// or include utils.h directly for the raw struct version.

#ifdef __cplusplus
}
#endif
