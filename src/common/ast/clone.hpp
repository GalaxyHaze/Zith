#pragma once

#include "common/ast/concepts.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"

#include <cstddef>
#include <type_traits>

namespace common::ast {

template <typename T, typename AstRoot>
concept HasAdlCloneInto = requires(AstRoot &target, T *source) {
    cloneInto(target, source);
};

template <typename T, typename AstRoot>
T *cloneNode(AstRoot &target, T *source) {
    if (source == nullptr)
        return nullptr;

    if constexpr (HasAdlCloneInto<T, AstRoot>) {
        return cloneInto(target, source);
    } else {
        auto *clone = target.arena->template make<T>(*source);
        if (clone == nullptr)
            return nullptr;

        if constexpr (requires(T *node) { for_each_child(node, [](auto &) {}); }) {
            common::memory::DynArray<void *> sourceChildren{*target.arena};
            for_each_child(source, [&](auto &childRef) {
                sourceChildren.push(&childRef);
            });

            std::size_t index = 0;
            for_each_child(clone, [&](auto &childRef) {
                using ChildRaw = std::remove_reference_t<decltype(childRef)>;
                auto *sourcePtr = static_cast<ChildRaw *>(sourceChildren[index++]);
                childRef = cloneNode(target, *sourcePtr);
            });
        }
        return clone;
    }
}

template <typename T, typename AstRoot>
T *cloneTree(AstRoot &target, T *sourceRoot) {
    return cloneNode(target, sourceRoot);
}

} // namespace common::ast
