#include "frontend/ast/ast.hpp"
#include "frontend/ast/walk.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using generated_ast::AstNode;
using generated_ast::AstRoot;
using generated_ast::Expr;
using generated_ast::Program;

namespace {

std::string capture_print(const AstRoot &ast) {
    FILE *out = tmpfile();
    if (out == nullptr)
        return {};
    generated_ast::print(ast, out);
    std::rewind(out);
    std::string text;
    char buffer[512];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), out)) != 0)
        text.append(buffer, n);
    std::fclose(out);
    return text;
}

} // namespace

int main() {
    common::memory::Arena arena;
    AstRoot ast(arena);

    Expr *callee = generated_ast::make<Expr>(
        ast, Span{6, 7}, 0, "", "callee", nullptr, nullptr);
    Expr *literal = generated_ast::make<Expr>(
        ast, Span{1, 2}, 0, "", "answer", nullptr, nullptr);
    Expr *call = generated_ast::make<Expr>(
        ast, Span{0, 6}, 0, "", "call", nullptr, nullptr);

    if (callee == nullptr || literal == nullptr || call == nullptr)
        return EXIT_FAILURE;

    call->operands.push(static_cast<AstNode *>(literal));
    call->operands.push(static_cast<AstNode *>(callee));

    Program *program = generated_ast::make<Program>(ast, Span{0, 7});
    if (program == nullptr)
        return EXIT_FAILURE;
    program->body.push(static_cast<AstNode *>(call));
    ast.root = program;

    if (ast.nodeCount() != 4)
        return EXIT_FAILURE;
    if (!ast.contains(program) || !ast.contains(call) ||
        !ast.contains(literal) || !ast.contains(callee))
        return EXIT_FAILURE;

    std::vector<const void *> visited;
    std::vector<const void *> parents;
    generated_ast::walk(ast, [&](auto *node, auto *parent) {
        visited.push_back(node);
        parents.push_back(parent);
    });

    if (visited.size() != 4)
        return EXIT_FAILURE;
    if (visited[0] != static_cast<AstNode *>(program) || parents[0] != nullptr)
        return EXIT_FAILURE;
    if (visited[1] != static_cast<AstNode *>(call) ||
        parents[1] != static_cast<AstNode *>(program))
        return EXIT_FAILURE;
    if (visited[2] != static_cast<AstNode *>(literal) ||
        parents[2] != static_cast<AstNode *>(call))
        return EXIT_FAILURE;
    if (visited[3] != static_cast<AstNode *>(callee) ||
        parents[3] != static_cast<AstNode *>(call))
        return EXIT_FAILURE;

    const std::string text = capture_print(ast);
    if (text.find("Program") == std::string::npos ||
        text.find("text=callee") == std::string::npos ||
        text.find("text=call") == std::string::npos ||
        text.find("text=answer") == std::string::npos)
        return EXIT_FAILURE;

    AstRoot moved{std::move(ast)};
    if (moved.nodeCount() != 4 || moved.root != program)
        return EXIT_FAILURE;
    if (ast.nodeCount() != 0 || ast.root != nullptr)
        return EXIT_FAILURE;
    generated_ast::free(moved);
    if (moved.nodeCount() != 0 || moved.root != nullptr)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
