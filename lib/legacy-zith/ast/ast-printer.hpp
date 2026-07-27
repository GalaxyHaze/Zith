#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"

#include <cstdio>

namespace zith::ast {

void printAST(const ProgramNode &program, const AstBuilder &builder, FILE *out = stdout);

} // namespace zith::ast
