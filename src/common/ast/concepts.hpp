#pragma once

namespace common::ast {

template <typename T, typename Fn>
concept HasChildVisitor = requires(T *node, Fn fn) {
    for_each_child(node, fn);
};

} // namespace common::ast
