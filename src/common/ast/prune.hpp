#pragma once

#include "common/ast/concepts.hpp"

namespace common::ast {

template <typename T>
bool pruneChild(T *parent, T *child) {
    if (parent == nullptr || child == nullptr)
        return false;

    bool removed = false;
    for_each_child(parent, [&](auto &candidateRef) {
        if (candidateRef == child) {
            candidateRef = nullptr;
            removed = true;
        }
    });
    return removed;
}

} // namespace common::ast
