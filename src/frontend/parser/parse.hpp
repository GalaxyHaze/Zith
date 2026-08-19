#pragma once

#include "frontend/parser/types.hpp"
#include "frontend/parser/parser.hpp"

#include <string_view>
#include <vector>

namespace hooks::parser {

[[nodiscard]] sample::ParseOutput parseSource(
    generated_parser::Parser<sample::ParseOutput> &parser,
    generated_lexer::TokenStream &tokens,
    std::string_view source,
    std::vector<sample::ParserDiagnostic> *outDiagnostics = nullptr);

} // namespace hooks::parser
