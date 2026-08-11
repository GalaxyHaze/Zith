#pragma once

#include "common/ast/concepts.hpp"

namespace common::ast {

template <typename T>
bool replaceChild(T *parent, T *oldNode, T *newNode) {
    if (parent == nullptr || oldNode == nullptr || newNode == nullptr)
        return false;

    bool replaced = false;
    for_each_child(parent, [&](auto &childRef) {
        if (childRef == oldNode) {
            childRef = newNode;
            replaced = true;
        }
    });
    return replaced;
}

} // namespace common::ast
