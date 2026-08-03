#pragma once

#include "frontend/frontend.hpp"

namespace zith::frontend {

void printTokens(const FrontendSnapshot &snapshot);
void printDeclarations(const FrontendSnapshot &snapshot);

} // namespace zith::frontend
