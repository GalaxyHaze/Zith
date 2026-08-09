#pragma once
#include "common/arena.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace zith::memory {

template <typename T> class DynArray {
    Arena *arena_    = nullptr;
    T *data_         = nullptr;
    size_t size_     = 0;
    size_t capacity_ = 0;

public:
    explicit DynArray(Arena &arena) noexcept : arena_(&arena) {}

    ~DynArray() noexcept {
        if (!data_)
            return;
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < size_; ++i)
                data_[i].~T();
        }
    }

    DynArray(const DynArray &)          = delete;
    auto operator=(const DynArray &) -> DynArray & = delete;

    DynArray(DynArray &&other) noexcept
        : arena_(other.arena_), data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_     = nullptr;
        other.size_     = 0;
        other.capacity_ = 0;
    }

    auto operator=(DynArray &&other) noexcept -> DynArray & {
        if (this != &other) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < size_; ++i)
                    data_[i].~T();
            }
            arena_          = other.arena_;
            data_           = other.data_;
            size_           = other.size_;
            capacity_       = other.capacity_;
            other.data_     = nullptr;
            other.size_     = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    void push(const T &value) {
        if (size_ >= capacity_)
            grow_();
        ::new (&data_[size_]) T(value);
        ++size_;
    }

    void push(T &&value) {
        if (size_ >= capacity_)
            grow_();
        ::new (&data_[size_]) T(std::move(value));
        ++size_;
    }

    void resize(size_t count) requires std::is_default_constructible_v<T> {
        if (count > size_) {
            reserve(count);
            while (size_ < count)
                emplace();
        } else if (count < size_) {
            shrink_(count);
        }
    }

    void resize(size_t count, const T &value) requires std::is_copy_constructible_v<T> {
        if (count > size_) {
            reserve(count);
            while (size_ < count)
                push(value);
        } else if (count < size_) {
            shrink_(count);
        }
    }

private:
    void shrink_(size_t count) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = count; i < size_; ++i)
                data_[i].~T();
        }
        size_ = count;
    }

public:
    template <typename... Args> void resizeEmplace(size_t count, Args &&...args) {
        if (count > size_) {
            reserve(count);
            while (size_ < count)
                emplace(std::forward<Args>(args)...);
        } else if (count < size_) {
            shrink_(count);
        }
    }

    void appendRange(const T *first, size_t count) requires std::is_copy_constructible_v<T> {
        if (count == 0)
            return;
        reserve(size_ + count);
        for (size_t i = 0; i < count; ++i)
            push(first[i]);
    }

    void appendRange(std::span<const T> values) requires std::is_copy_constructible_v<T> {
        appendRange(values.data(), values.size());
    }

    template <typename... Args> auto &emplace(Args &&...args) {
        if (size_ >= capacity_)
            grow_();
        auto *ptr = ::new (&data_[size_]) T(std::forward<Args>(args)...);
        ++size_;
        return *ptr;
    }

    void reserve(size_t new_cap) {
        if (new_cap <= capacity_)
            return;
        if (new_cap > std::numeric_limits<size_t>::max() / sizeof(T))
            allocationFailure_();

        const size_t old_bytes = capacity_ * sizeof(T);
        const size_t new_bytes = new_cap * sizeof(T);
        const size_t extra     = new_bytes - old_bytes;

        auto *data_end = data_ ? reinterpret_cast<char *>(data_) + old_bytes : nullptr;
        if (data_end && data_end == arena_->ptr() && arena_->remaining() >= extra) {
            (void)arena_->alloc(extra, alignof(T));
            capacity_ = new_cap;
            return;
        }

        auto *new_data = static_cast<T *>(arena_->alloc(new_bytes, alignof(T)));
        if (!new_data)
            allocationFailure_();

        if (data_) {
            for (size_t i = 0; i < size_; ++i) {
                ::new (&new_data[i]) T(std::move(data_[i]));
                data_[i].~T();
            }
        }

        data_     = new_data;
        capacity_ = new_cap;
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
        return size_;
    }

    [[nodiscard]] auto capacity() const noexcept -> size_t {
        return capacity_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < size_; ++i)
                data_[i].~T();
        }
        size_ = 0;
    }

    void pop_back() {
        if (size_ == 0)
            return;
        if constexpr (!std::is_trivially_destructible_v<T>)
            data_[size_ - 1].~T();
        --size_;
    }

    auto back() noexcept -> T & {
        if (size() == 0) {
            std::fprintf(stderr, "[error] attemp to use 'back' on DynArray siz 0");
            std::abort();
        }
        return data_[size_ - 1];
    }

    auto back() const noexcept -> const T & {
        if (size() == 0) {
            std::fprintf(stderr, "[error] attemp to use 'back' on DynArray siz 0");
            std::abort();
        }
        return data_[size_ - 1];
    }

    auto operator[](size_t index) -> T & {
        return data_[index];
    }

    auto operator[](size_t index) const -> const T & {
        return data_[index];
    }

    auto at(size_t index) noexcept -> T & {
        if (index >= size_) {
            std::fprintf(stderr, "[error] DynArray::at index out of range: %zu >= %zu\n", index,
                         size_);
            std::abort();
        }
        return data_[index];
    }

    auto at(size_t index) const noexcept -> const T & {
        if (index >= size_) {
            std::fprintf(stderr, "[error] DynArray::at index out of range: %zu >= %zu\n", index,
                         size_);
            std::abort();
        }
        return data_[index];
    }

    auto begin() noexcept -> T * {
        return data_;
    }

    auto end() noexcept -> T * {
        return data_ + size_;
    }

    auto begin() const noexcept -> const T * {
        return data_;
    }

    auto end() const noexcept -> const T * {
        return data_ + size_;
    }

    auto data() noexcept -> T * {
        return data_;
    }

    auto data() const noexcept -> const T * {
        return data_;
    }

    auto arena() noexcept -> Arena & {
        return *arena_;
    }

private:
    void grow_() {
        if (capacity_ > std::numeric_limits<size_t>::max() / 2)
            allocationFailure_();
        const size_t new_cap = capacity_ == 0 ? 4 : capacity_ * 2;
        reserve(new_cap);
    }

    [[noreturn]] static void allocationFailure_() {
        std::fputs("[error] DynArray allocation failed or overflowed\n", stderr);
        std::abort();
    }
};

template <class T> auto makeDynArray(Arena &allocator) {
    return DynArray<T>(allocator);
}

} // namespace zith::memory
