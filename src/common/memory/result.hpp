#pragma once

#include <concepts>
#include <cstdlib>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace common::memory {

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

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) & -> Result<std::invoke_result_t<Function, T &>, E> {
        using U = std::invoke_result_t<Function, T &>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function), value());
        return Result<U, E>(error());
    }

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) const &
        -> Result<std::invoke_result_t<Function, const T &>, E> {
        using U = std::invoke_result_t<Function, const T &>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function), value());
        return Result<U, E>(error());
    }

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) &&
        -> Result<std::invoke_result_t<Function, T &&>, E> {
        using U = std::invoke_result_t<Function, T &&>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function), std::move(*this).value());
        return Result<U, E>(std::move(*this).error());
    }

    template <typename Function> [[nodiscard]] auto andThen(Function &&function) & -> std::invoke_result_t<Function, T &> {
        using Next = std::invoke_result_t<Function, T &>;
        if (isOk())
            return std::invoke(std::forward<Function>(function), value());
        return Next(error());
    }

    template <typename Function>
    [[nodiscard]] auto andThen(Function &&function) const & -> std::invoke_result_t<Function, const T &> {
        using Next = std::invoke_result_t<Function, const T &>;
        if (isOk())
            return std::invoke(std::forward<Function>(function), value());
        return Next(error());
    }

    template <typename Function> [[nodiscard]] auto andThen(Function &&function) && -> std::invoke_result_t<Function, T &&> {
        using Next = std::invoke_result_t<Function, T &&>;
        if (isOk())
            return std::invoke(std::forward<Function>(function), std::move(*this).value());
        return Next(std::move(*this).error());
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) & {
        return andThen(std::forward<Function>(function));
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) const & {
        return andThen(std::forward<Function>(function));
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) && {
        return std::move(*this).andThen(std::forward<Function>(function));
    }

private:
    template <typename U, typename Function, typename Value>
    static auto invokeMap_(Function &&function, Value &&value) -> Result<U, E> {
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<Function>(function), std::forward<Value>(value));
            return Result<void, E>{};
        } else {
            return Result<U, E>(
                std::invoke(std::forward<Function>(function), std::forward<Value>(value)));
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

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) & -> Result<std::invoke_result_t<Function>, E> {
        using U = std::invoke_result_t<Function>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function));
        return Result<U, E>(error());
    }

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) const & -> Result<std::invoke_result_t<Function>, E> {
        using U = std::invoke_result_t<Function>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function));
        return Result<U, E>(error());
    }

    template <typename Function>
    [[nodiscard]] auto map(Function &&function) && -> Result<std::invoke_result_t<Function>, E> {
        using U = std::invoke_result_t<Function>;
        if (isOk())
            return invokeMap_<U>(std::forward<Function>(function));
        return Result<U, E>(std::move(*this).error());
    }

    template <typename Function> [[nodiscard]] auto andThen(Function &&function) & -> std::invoke_result_t<Function> {
        using Next = std::invoke_result_t<Function>;
        if (isOk())
            return std::invoke(std::forward<Function>(function));
        return Next(error());
    }

    template <typename Function>
    [[nodiscard]] auto andThen(Function &&function) const & -> std::invoke_result_t<Function> {
        using Next = std::invoke_result_t<Function>;
        if (isOk())
            return std::invoke(std::forward<Function>(function));
        return Next(error());
    }

    template <typename Function> [[nodiscard]] auto andThen(Function &&function) && -> std::invoke_result_t<Function> {
        using Next = std::invoke_result_t<Function>;
        if (isOk())
            return std::invoke(std::forward<Function>(function));
        return Next(std::move(*this).error());
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) & {
        return andThen(std::forward<Function>(function));
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) const & {
        return andThen(std::forward<Function>(function));
    }

    template <typename Function> [[nodiscard]] auto and_then(Function &&function) && {
        return std::move(*this).andThen(std::forward<Function>(function));
    }

private:
    template <typename U, typename Function> static auto invokeMap_(Function &&function) -> Result<U, E> {
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<Function>(function));
            return Result<void, E>{};
        } else {
            return Result<U, E>(std::invoke(std::forward<Function>(function)));
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

} // namespace common::memory
