#pragma once
#include <cassert>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace zith::memory {

class Arena {
public:
    static constexpr size_t default_block_size = 4096;

private:
    struct DestructorEntry {
        DestructorEntry *previous        = nullptr;
        void *object                     = nullptr;
        void (*destroy)(void *) noexcept = nullptr;
    };

public:
    struct Block {
        Block *next = nullptr;
        Block *prev = nullptr;
        size_t size = 0; // usable data size (not including Block header and alignment padding)
        size_t dataOffset          = 0;
        size_t allocationAlignment = alignof(Block);
    };

    Arena() : Arena(default_block_size) {}

    explicit Arena(const size_t block_size)
        : block_size_(block_size == 0 ? default_block_size : block_size) {
        (void)addBlock_(block_size_, alignof(std::max_align_t));
    }

    ~Arena() noexcept {
        clear();
    }

    Arena(const Arena &)          = delete;
    auto operator=(const Arena &) = delete;

    Arena(Arena &&other) noexcept
        : head_(other.head_), current_(other.current_), offset_(other.offset_),
          block_size_(other.block_size_), destructors_(other.destructors_) {
        other.head_        = nullptr;
        other.current_     = nullptr;
        other.offset_      = 0;
        other.destructors_ = nullptr;
    }

    auto operator=(Arena &&other) noexcept -> Arena & {
        if (this != &other) {
            clear();
            head_              = other.head_;
            current_           = other.current_;
            offset_            = other.offset_;
            block_size_        = other.block_size_;
            destructors_       = other.destructors_;
            other.head_        = nullptr;
            other.current_     = nullptr;
            other.offset_      = 0;
            other.destructors_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] auto alloc(size_t size, size_t alignment = alignof(std::max_align_t)) -> void * {
        if (!isPowerOfTwo_(alignment))
            return nullptr;
        if (size == 0)
            size = 1;

        size_t aligned_offset = 0;
        if (!tryAlignUp_(offset_, alignment, aligned_offset))
            return nullptr;

        if (!current_ || current_->allocationAlignment < alignment ||
            size > current_->size - aligned_offset) {
            if (!addBlock_(size, alignment))
                return nullptr;
            aligned_offset = 0;
        }

        if (size > current_->size - aligned_offset)
            return nullptr;
        offset_ = aligned_offset + size;
        return data_(current_) + aligned_offset;
    }

    template <typename T, typename... Args> [[nodiscard]] auto make(Args &&...args) -> T * {
        static_assert(!std::is_array_v<T>, "Arena::make does not create arrays");

        DestructorEntry *entry = nullptr;
        if constexpr (!std::is_trivially_destructible_v<T>) {
            entry = static_cast<DestructorEntry *>(
                alloc(sizeof(DestructorEntry), alignof(DestructorEntry)));
            if (!entry)
                return nullptr;
        }

        auto *object = static_cast<T *>(alloc(sizeof(T), alignof(T)));
        if (!object)
            return nullptr;

        ::new (object) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            entry->previous = destructors_;
            entry->object   = object;
            entry->destroy  = [](void *value) noexcept { static_cast<T *>(value)->~T(); };
            destructors_    = entry;
        }
        return object;
    }

    [[nodiscard]] auto ptr() const noexcept -> const void * {
        return current_ ? data_(current_) + offset_ : nullptr;
    }

    [[nodiscard]] auto remaining() const noexcept -> size_t {
        return current_ ? (current_->size - offset_) : 0;
    }

    /// Total usable data capacity across all allocated blocks (excludes Block header overhead).
    [[nodiscard]] size_t allocatedBytes() const noexcept {
        size_t total = 0;
        for (const Block *b = head_; b; b = b->next)
            total += b->size;
        return total;
    }

    /// Bytes actually written: full prior blocks plus the current write offset.
    [[nodiscard]] size_t usedBytes() const noexcept {
        if (!head_)
            return 0;
        size_t total = 0;
        for (const Block *b = head_; b != current_; b = b->next)
            total += b->size;
        total += offset_;
        return total;
    }

    void clear() noexcept {
        destroyUntil_(nullptr);
        auto *block = head_;
        while (block) {
            auto *next = block->next;
            ::operator delete(block, std::align_val_t(block->allocationAlignment));
            block = next;
        }
        head_    = nullptr;
        current_ = nullptr;
        offset_  = 0;
    }

    friend class MarkPoint;

private:
    Block *head_                  = nullptr;
    Block *current_               = nullptr;
    size_t offset_                = 0;
    size_t block_size_            = default_block_size;
    DestructorEntry *destructors_ = nullptr;

    [[nodiscard]] bool addBlock_(size_t min_size, size_t alignment) noexcept {
        if (min_size < block_size_) {
            min_size = block_size_;
        }
        const size_t allocation_alignment = alignment > alignof(Block) ? alignment : alignof(Block);
        size_t data_offset                = 0;
        if (!tryAlignUp_(sizeof(Block), allocation_alignment, data_offset))
            return false;
        if (min_size > std::numeric_limits<size_t>::max() - data_offset)
            return false;

        const size_t total = data_offset + min_size;
        auto *mem = ::operator new(total, std::align_val_t(allocation_alignment), std::nothrow);
        if (!mem)
            return false;
        auto *block                = ::new (mem) Block{};
        block->size                = min_size;
        block->dataOffset          = data_offset;
        block->allocationAlignment = allocation_alignment;
        block->prev                = current_;

        if (current_) {
            current_->next = block;
        } else {
            head_ = block;
        }
        current_ = block;
        offset_  = 0;
        return true;
    }

    static auto data_(Block *block) noexcept -> char * {
        return reinterpret_cast<char *>(block) + block->dataOffset;
    }

    static constexpr bool isPowerOfTwo_(size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    static bool tryAlignUp_(size_t value, size_t alignment, size_t &result) noexcept {
        assert(isPowerOfTwo_(alignment));
        const size_t mask = alignment - 1;
        if (value > std::numeric_limits<size_t>::max() - mask)
            return false;
        result = (value + mask) & ~mask;
        return true;
    }

    void destroyUntil_(DestructorEntry *limit) noexcept {
        while (destructors_ != limit) {
            auto *entry  = destructors_;
            destructors_ = entry->previous;
            entry->destroy(entry->object);
        }
    }
};

class MarkPoint {
    Arena *arena_                        = nullptr;
    Arena::Block *block_                 = nullptr;
    size_t offset_                       = 0;
    Arena::DestructorEntry *destructors_ = nullptr;

public:
    explicit MarkPoint(Arena &arena) noexcept
        : arena_(&arena), block_(arena.current_), offset_(arena.offset_),
          destructors_(arena.destructors_) {}

    ~MarkPoint() noexcept {
        if (arena_) {
            rollback();
        }
    }

    MarkPoint(const MarkPoint &)      = delete;
    auto operator=(const MarkPoint &) = delete;

    MarkPoint(MarkPoint &&other) noexcept
        : arena_(other.arena_), block_(other.block_), offset_(other.offset_),
          destructors_(other.destructors_) {
        other.arena_ = nullptr;
    }

    auto operator=(MarkPoint &&other) noexcept -> MarkPoint & {
        if (this != &other) {
            if (arena_) {
                rollback();
            }
            arena_       = other.arena_;
            block_       = other.block_;
            offset_      = other.offset_;
            destructors_ = other.destructors_;
            other.arena_ = nullptr;
        }
        return *this;
    }

    void rollback() noexcept {
        auto *arena = arena_;
        arena_      = nullptr;
        if (!arena)
            return;

        arena->destroyUntil_(destructors_);

        auto *block = block_;
        auto offset = offset_;

        auto *to_free = block ? block->next : arena->head_;
        while (to_free) {
            auto *next = to_free->next;
            ::operator delete(to_free, std::align_val_t(to_free->allocationAlignment));
            to_free = next;
        }

        if (block) {
            block->next     = nullptr;
            arena->current_ = block;
        } else {
            arena->head_    = nullptr;
            arena->current_ = nullptr;
        }

        arena->offset_ = offset;
    }

    void release() noexcept {
        arena_ = nullptr;
    }
};

} // namespace zith::memory
