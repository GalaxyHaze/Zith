#pragma once

#include "common/ast/concepts.hpp"

namespace common::ast {

template <typename T, typename AstRoot, typename Fn>
    requires HasChildVisitor<T, Fn>
T *transformNode(AstRoot &ast, T *node, T *parent, Fn &fn) {
    if (node == nullptr)
        return nullptr;

    for_each_child(node, [&](auto &childRef) { childRef = transformNode(ast, childRef, node, fn); });

    return fn(node, parent);
}

template <typename T, typename AstRoot, typename Fn>
    requires(!HasChildVisitor<T, Fn>)
T *transformNode(AstRoot &ast, T *node, T *parent, Fn &fn) {
    if (node == nullptr)
        return nullptr;
    return fn(node, parent);
}

template <typename T, typename AstRoot, typename Fn>
    requires HasChildVisitor<T, Fn>
T *transform(AstRoot &ast, T *root, Fn &&fn) {
    return transformNode(ast, root, static_cast<T *>(nullptr), fn);
}

} // namespace common::ast
