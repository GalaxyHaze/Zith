#pragma once

#include "common/ast/concepts.hpp"

namespace common::ast {

template <typename Parent, typename Fn>
    requires HasChildVisitor<Parent, Fn>
void visitChildren(Parent *parent, Fn &&fn) {
    if (parent == nullptr)
        return;
    for_each_child(parent, std::forward<Fn>(fn));
}

template <typename T, typename Parent, typename Fn>
    requires HasChildVisitor<T, Fn>
void visitNode(T *node, Parent *parent, Fn &fn) {
    if (node == nullptr)
        return;

    fn(node, parent);
    visitChildren(node, [&](auto &childRef) { visitNode(childRef, node, fn); });
}

template <typename T, typename Parent, typename Fn>
    requires(!HasChildVisitor<T, Fn>)
void visitNode(T *node, Parent *parent, Fn &fn) {
    if (node == nullptr)
        return;
    fn(node, parent);
}

template <typename T, typename Fn>
    requires HasChildVisitor<T, Fn>
void visit(T *root, Fn &&fn) {
    if (root == nullptr)
        return;
    visitNode(root, static_cast<T *>(nullptr), fn);
}

} // namespace common::ast
