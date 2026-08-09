#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace zith::memory {

template <class T> class Optional {
    alignas(T) char data[sizeof(T)] = {};
    bool valid{false};

public:
    Optional() = default;

    Optional(T &&value) : valid(true) {
        new (data) T(std::move(value));
    }

    Optional(const T &value) : valid(true) {
        new (data) T(value);
    }

    template <class... Args> explicit Optional(Args &&...args) : valid(true) {
        new (data) T(std::forward<Args>(args)...);
    }

    Optional(std::nullptr_t) : valid(false) {}

    Optional(const Optional &other) : valid(other.valid) {
        if (valid)
            new (data) T(other.value());
    }

    Optional(Optional &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : valid(other.valid) {
        if (valid)
            new (data) T(std::move(other.value()));
    }

    auto operator=(const Optional &other) -> Optional & {
        if (this != &other) {
            reset();
            if (other.valid) {
                new (data) T(other.value());
                valid = true;
            }
        }
        return *this;
    }

    auto operator=(Optional &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
        -> Optional & {
        if (this != &other) {
            reset();
            if (other.valid) {
                new (data) T(std::move(other.value()));
                valid = true;
            }
        }
        return *this;
    }

    ~Optional() {
        reset();
    }

    void reset() noexcept {
        if (valid && !std::is_trivially_destructible_v<T>)
            std::launder(reinterpret_cast<T *>(data))->~T();
        valid = false;
    }

    bool isValid() const noexcept {
        return valid;
    }

    bool isEmpty() const noexcept {
        return !valid;
    }

    explicit operator bool() const noexcept {
        return valid;
    }

    T &value() & {
        requireValue_();
        return *std::launder(reinterpret_cast<T *>(data));
    }

    const T &value() const & {
        requireValue_();
        return *std::launder(reinterpret_cast<const T *>(data));
    }

    T &&value() && {
        return std::move(value());
    }

    const T &&value() const && {
        return std::move(value());
    }

    T *operator->() {
        return &value();
    }

    const T *operator->() const {
        return &value();
    }

    T &operator*() & {
        return value();
    }

    const T &operator*() const & {
        return value();
    }

    T &&operator*() && {
        return std::move(value());
    }

    const T &&operator*() const && {
        return std::move(value());
    }

private:
    void requireValue_() const noexcept {
        if (!valid)
            std::abort();
    }
};

template <class T> class Optional<T *> {
    T *data = nullptr;

public:
    Optional() = default;
    Optional(T *value) : data(value) {}
    explicit Optional(std::nullptr_t) : data(nullptr) {}

    bool isValid() const noexcept {
        return data != nullptr;
    }

    bool isEmpty() const noexcept {
        return data == nullptr;
    }

    explicit operator bool() const noexcept {
        return data != nullptr;
    }

    T &value() & {
        if (!data)
            std::abort();
        return *data;
    }

    const T &value() const & {
        if (!data)
            std::abort();
        return *data;
    }

    T &&value() && {
        if (!data)
            std::abort();
        return std::move(*data);
    }

    const T &&value() const && {
        if (!data)
            std::abort();
        return std::move(*data);
    }

    T *get() const noexcept {
        return data;
    }

    T *operator->() {
        return data;
    }

    const T *operator->() const {
        return data;
    }

    T &operator*() & {
        return value();
    }

    const T &operator*() const & {
        return value();
    }

    T &&operator*() && {
        return std::move(value());
    }

    const T &&operator*() const && {
        return std::move(value());
    }
};

} // namespace zith::memory
