#pragma once

#include "common/ast/concepts.hpp"

namespace common::ast {

template <typename Parent, typename OldNode, typename NewNode>
bool replaceChild(Parent *parent, OldNode *oldNode, NewNode *newNode) {
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
