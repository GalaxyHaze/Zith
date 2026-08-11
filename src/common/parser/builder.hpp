#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace common::parser {

namespace detail {

template <typename Parent, typename Child>
void attachChild(Parent &parent, Child child) {
    attach(parent, std::move(child));
}

} // namespace detail

template <typename Output>
class OutputBuilder {
public:
    explicit OutputBuilder(memory::Arena &arena) : stack_(arena) {}

    static_assert(requires(Output &parent, Output child) {
                      attach(parent, child);
                  },
                  "OutputBuilder requires an ADL attach(parent, child) function");

    void push(Output value) {
        stack_.push(std::move(value));
    }

    Output pop() {
        Output value = std::move(stack_.back());
        stack_.pop_back();
        return value;
    }

    [[nodiscard]] Output &top() noexcept {
        return stack_.back();
    }

    [[nodiscard]] const Output &top() const noexcept {
        return stack_.back();
    }

    [[nodiscard]] bool empty() const noexcept {
        return stack_.empty();
    }

    [[nodiscard]] size_t size() const noexcept {
        return stack_.size();
    }

    // Pops top two; attaches child (old top) to parent (new top)
    // via ADL: attach(parent, child).
    void attach() {
        if (stack_.size() < 2)
            abortStackUnderflow();
        Output child = pop();
        detail::attachChild(stack_.back(), std::move(child));
    }

    // Pops `count` elements and attaches them as children to the new top.
    // count=0 attaches the current top to the element below it.
    void close(unsigned count = 0) {
        if (count == 0)
            count = 1;
        if (stack_.size() < static_cast<size_t>(count) + 1)
            abortStackUnderflow();
        for (unsigned i = 0; i < count; ++i) {
            Output child = pop();
            detail::attachChild(stack_.back(), std::move(child));
        }
    }

private:
    [[noreturn]] static void abortStackUnderflow() {
        std::fprintf(stderr,
                     "[error] OutputBuilder attach/close requires stack depth >= 2\n");
        std::abort();
    }

    memory::DynArray<Output> stack_;
};

} // namespace common::parser
