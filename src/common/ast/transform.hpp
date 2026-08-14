#pragma once

#include "common/ast/concepts.hpp"
#include "common/ast/visit.hpp"

#include <type_traits>
#include <utility>

namespace common::ast {

template <typename T, typename AstRoot, typename Fn>
    requires HasChildVisitor<T, Fn>
T *transformNode(AstRoot &ast, T *node, T *parent, Fn &&fn) {
    if (node == nullptr)
        return nullptr;

    visitChildren(node, [&](auto &childRef) {
        using Child = std::remove_pointer_t<std::remove_reference_t<decltype(childRef)>>;
        childRef = transformNode(ast, childRef, node, fn);
    });

    return fn(node, parent);
}

template <typename T, typename AstRoot, typename Fn>
    requires(!HasChildVisitor<T, Fn>)
T *transformNode(AstRoot &ast, T *node, T *parent, Fn &&fn) {
    if (node == nullptr)
        return nullptr;
    return fn(node, parent);
}

template <typename T, typename AstRoot, typename Fn>
    requires HasChildVisitor<T, Fn>
T *transform(AstRoot &ast, T *root, Fn &&fn) {
    return transformNode(ast, root, static_cast<T *>(nullptr), std::forward<Fn>(fn));
}

} // namespace common::ast
