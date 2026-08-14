#include "frontend/ast/ast.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/span.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::memory::Arena;
using common::memory::Span;

int main() {
    Arena arena;
    generated_ast::AstRoot ast(arena);

    auto *program = generated_ast::make<generated_ast::Program>(ast);
    auto *literal = generated_ast::make<generated_ast::LiteralExpr>(
        ast, Span{0, 1}, "42");
    auto *expr = generated_ast::make<generated_ast::Expr>(ast, Span{0, 1});
    auto *call = generated_ast::make<generated_ast::CallExpr>(
        ast, Span{0, 8}, expr, "answer");

    call->arguments.push(literal);
    program->body.push(call);

    ast.root = program;
    if (ast.root == nullptr || ast.nodeCount() != 4 || !ast.contains(call))
        return EXIT_FAILURE;

    generated_ast::print(ast, stdout);
    std::cout << "ast-demo: Program -> CallExpr -> LiteralExpr printed\n";
    return EXIT_SUCCESS;
}
