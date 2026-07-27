#pragma once

#include <concepts>
#include <cstdlib>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace zith::memory {

struct Error {
    std::string msg;
};

template <class T>
concept Failable = std::derived_from<T, Error>;

template <class T, Failable E = Error> class Result {
    static_assert(!std::is_void_v<T>, "Result<void, E> has its own specialization");
    static_assert(!std::is_same_v<T, E>, "Result<T, E> requires T and E to be different types");

    std::variant<T, E> data_;

public:
    Result(T &&value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(const T &value) : data_(std::in_place_index<0>, value) {}
    Result(E &&error) : data_(std::in_place_index<1>, std::move(error)) {}
    Result(const E &error) : data_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool isOk() const noexcept {
        return data_.index() == 0;
    }
    [[nodiscard]] bool isError() const noexcept {
        return !isOk();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return isOk();
    }

    [[nodiscard]] T &value() & {
        return *valuePtr_();
    }
    [[nodiscard]] const T &value() const & {
        return *valuePtr_();
    }
    [[nodiscard]] T &&value() && {
        return std::move(*valuePtr_());
    }

    [[nodiscard]] E &error() & {
        return *errorPtr_();
    }
    [[nodiscard]] const E &error() const & {
        return *errorPtr_();
    }
    [[nodiscard]] E &&error() && {
        return std::move(*errorPtr_());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) & -> Result<std::invoke_result_t<F, T &>, E> {
        using U = std::invoke_result_t<F, T &>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function), value());
        return Result<U, E>(error());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) const & -> Result<std::invoke_result_t<F, const T &>, E> {
        using U = std::invoke_result_t<F, const T &>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function), value());
        return Result<U, E>(error());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) && -> Result<std::invoke_result_t<F, T &&>, E> {
        using U = std::invoke_result_t<F, T &&>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function), std::move(*this).value());
        return Result<U, E>(std::move(*this).error());
    }

    template <typename F>
    [[nodiscard]] auto andThen(F &&function) & -> std::invoke_result_t<F, T &> {
        using Next = std::invoke_result_t<F, T &>;
        if (isOk())
            return std::invoke(std::forward<F>(function), value());
        return Next(error());
    }

    template <typename F>
    [[nodiscard]] auto andThen(F &&function) const & -> std::invoke_result_t<F, const T &> {
        using Next = std::invoke_result_t<F, const T &>;
        if (isOk())
            return std::invoke(std::forward<F>(function), value());
        return Next(error());
    }

    template <typename F>
    [[nodiscard]] auto andThen(F &&function) && -> std::invoke_result_t<F, T &&> {
        using Next = std::invoke_result_t<F, T &&>;
        if (isOk())
            return std::invoke(std::forward<F>(function), std::move(*this).value());
        return Next(std::move(*this).error());
    }

    template <typename F> [[nodiscard]] auto and_then(F &&function) & {
        return andThen(std::forward<F>(function));
    }
    template <typename F> [[nodiscard]] auto and_then(F &&function) const & {
        return andThen(std::forward<F>(function));
    }
    template <typename F> [[nodiscard]] auto and_then(F &&function) && {
        return std::move(*this).andThen(std::forward<F>(function));
    }

private:
    template <typename U, typename F, typename Value>
    static auto invokeMap_(F &&function, Value &&value) -> Result<U, E> {
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(function), std::forward<Value>(value));
            return Result<void, E>{};
        } else {
            return Result<U, E>(std::invoke(std::forward<F>(function), std::forward<Value>(value)));
        }
    }

    [[nodiscard]] T *valuePtr_() noexcept {
        auto *value = std::get_if<0>(&data_);
        if (!value)
            std::abort();
        return value;
    }

    [[nodiscard]] const T *valuePtr_() const noexcept {
        const auto *value = std::get_if<0>(&data_);
        if (!value)
            std::abort();
        return value;
    }

    [[nodiscard]] E *errorPtr_() noexcept {
        auto *error = std::get_if<1>(&data_);
        if (!error)
            std::abort();
        return error;
    }

    [[nodiscard]] const E *errorPtr_() const noexcept {
        const auto *error = std::get_if<1>(&data_);
        if (!error)
            std::abort();
        return error;
    }
};

template <Failable E> class Result<void, E> {
    std::variant<std::monostate, E> data_;

public:
    Result() : data_(std::in_place_index<0>) {}
    Result(E &&error) : data_(std::in_place_index<1>, std::move(error)) {}
    Result(const E &error) : data_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool isOk() const noexcept {
        return data_.index() == 0;
    }
    [[nodiscard]] bool isError() const noexcept {
        return !isOk();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return isOk();
    }

    void value() const {
        if (!isOk())
            std::abort();
    }

    [[nodiscard]] E &error() & {
        return *errorPtr_();
    }
    [[nodiscard]] const E &error() const & {
        return *errorPtr_();
    }
    [[nodiscard]] E &&error() && {
        return std::move(*errorPtr_());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) & -> Result<std::invoke_result_t<F>, E> {
        using U = std::invoke_result_t<F>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function));
        return Result<U, E>(error());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) const & -> Result<std::invoke_result_t<F>, E> {
        using U = std::invoke_result_t<F>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function));
        return Result<U, E>(error());
    }

    template <typename F>
    [[nodiscard]] auto map(F &&function) && -> Result<std::invoke_result_t<F>, E> {
        using U = std::invoke_result_t<F>;
        if (isOk())
            return invokeMap_<U>(std::forward<F>(function));
        return Result<U, E>(std::move(*this).error());
    }

    template <typename F> [[nodiscard]] auto andThen(F &&function) & -> std::invoke_result_t<F> {
        using Next = std::invoke_result_t<F>;
        if (isOk())
            return std::invoke(std::forward<F>(function));
        return Next(error());
    }

    template <typename F>
    [[nodiscard]] auto andThen(F &&function) const & -> std::invoke_result_t<F> {
        using Next = std::invoke_result_t<F>;
        if (isOk())
            return std::invoke(std::forward<F>(function));
        return Next(error());
    }

    template <typename F> [[nodiscard]] auto andThen(F &&function) && -> std::invoke_result_t<F> {
        using Next = std::invoke_result_t<F>;
        if (isOk())
            return std::invoke(std::forward<F>(function));
        return Next(std::move(*this).error());
    }

    template <typename F> [[nodiscard]] auto and_then(F &&function) & {
        return andThen(std::forward<F>(function));
    }
    template <typename F> [[nodiscard]] auto and_then(F &&function) const & {
        return andThen(std::forward<F>(function));
    }
    template <typename F> [[nodiscard]] auto and_then(F &&function) && {
        return std::move(*this).andThen(std::forward<F>(function));
    }

private:
    template <typename U, typename F> static auto invokeMap_(F &&function) -> Result<U, E> {
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(function));
            return Result<void, E>{};
        } else {
            return Result<U, E>(std::invoke(std::forward<F>(function)));
        }
    }

    [[nodiscard]] E *errorPtr_() noexcept {
        auto *error = std::get_if<1>(&data_);
        if (!error)
            std::abort();
        return error;
    }

    [[nodiscard]] const E *errorPtr_() const noexcept {
        const auto *error = std::get_if<1>(&data_);
        if (!error)
            std::abort();
        return error;
    }
};

} // namespace zith::memory
