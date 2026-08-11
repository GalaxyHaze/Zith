#include "common/ast/clone.hpp"
#include "common/ast/prune.hpp"
#include "common/ast/replace.hpp"
#include "common/ast/transform.hpp"
#include "common/ast/visit.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/span.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Expr {
  enum class Kind { Lit, Bin };

  Kind kind = Kind::Lit;
  int value = 0;
  common::memory::Span span{};
  Expr *left = nullptr;
  Expr *right = nullptr;
};

template <typename Fn> void for_each_child(Expr *node, Fn &&fn) {
  if (node->kind == Expr::Kind::Bin) {
    fn(node->left);
    fn(node->right);
  }
}

struct AstRoot {
  explicit AstRoot(common::memory::Arena &a) : arena(&a) {}

  common::memory::Arena *arena = nullptr;
};

template <typename Fn> void visitTree(Expr *root, Fn &&fn) {
  if (root == nullptr)
    return;
  fn(root);
  visitTree(root->left, fn);
  visitTree(root->right, fn);
}

bool compareTrees(Expr *a, Expr *b) {
  if (a == nullptr || b == nullptr)
    return a == b;
  if (a->kind != b->kind || a->value != b->value)
    return false;
  return compareTrees(a->left, b->left) && compareTrees(a->right, b->right);
}

Expr *makeLit(AstRoot &ast, int value, common::memory::Span span = {}) {
  auto *node = ast.arena->make<Expr>();
  node->kind = Expr::Kind::Lit;
  node->value = value;
  node->span = span;
  return node;
}

Expr *makeBin(AstRoot &ast, Expr *left, Expr *right,
              common::memory::Span span = {}) {
  auto *node = ast.arena->make<Expr>();
  node->kind = Expr::Kind::Bin;
  node->value = 0;
  node->span = span;
  node->left = left;
  node->right = right;
  return node;
}

bool runCloneTest() {
  common::memory::Arena arena;
  AstRoot source(arena);
  AstRoot target(arena);

  auto *left = makeLit(source, 42);
  auto *right = makeLit(source, 7);
  auto *root = makeBin(source, left, right);

  auto *copy = common::ast::cloneTree(target, root);
  if (!compareTrees(root, copy))
    return false;

  left->value = 1;
  auto *copyLeft = copy->left;
  return copyLeft != nullptr && copyLeft->value == 42;
}

bool runReplaceTest() {
  common::memory::Arena arena;
  AstRoot ast(arena);

  auto *oldNode = makeLit(ast, 42);
  auto *repl = makeLit(ast, 0);
  auto *root = makeBin(ast, oldNode, makeLit(ast, 7));

  if (!common::ast::replaceChild(root, oldNode, repl))
    return false;
  return root->left == repl;
}

bool runPruneTest() {
  common::memory::Arena arena;
  AstRoot ast(arena);

  auto *oldNode = makeLit(ast, 42);
  auto *root = makeBin(ast, oldNode, makeLit(ast, 7));

  if (!common::ast::pruneChild(root, oldNode))
    return false;
  return root->left == nullptr;
}

bool runTransformTest() {
  common::memory::Arena arena;
  AstRoot ast(arena);

  auto *root = makeBin(ast, makeLit(ast, 42), makeLit(ast, 42));

  auto *result =
      common::ast::transform(ast, root, [&](Expr *node, Expr *) -> Expr * {
        if (node->kind == Expr::Kind::Lit && node->value == 42)
          return makeLit(ast, 0);
        return node;
      });

  bool allZero = true;
  visitTree(result, [&](Expr *node) {
    if (node->value != 0)
      allZero = false;
  });
  return result != nullptr && allZero;
}

bool runVisitTest() {
  common::memory::Arena arena;
  AstRoot ast(arena);

  auto *left = makeLit(ast, 1);
  auto *right = makeLit(ast, 2);
  auto *root = makeBin(ast, left, right);
  root->value = 0;

  std::vector<int> order;
  std::vector<Expr *> parents;
  common::ast::visit(root, [&](Expr *node, Expr *parent) {
    order.push_back(node->value);
    parents.push_back(parent);
  });

  if (order.size() != 3)
    return false;
  if (order[0] != 0 || order[1] != 1 || order[2] != 2)
    return false;
  return parents[0] == nullptr && parents[1] == root && parents[2] == root;
}

} // namespace

int main() {
  bool ok = true;
  ok = ok && runCloneTest();
  ok = ok && runReplaceTest();
  ok = ok && runPruneTest();
  ok = ok && runTransformTest();
  ok = ok && runVisitTest();

  if (!ok) {
    std::fprintf(stderr, "ast helpers basics failed\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
