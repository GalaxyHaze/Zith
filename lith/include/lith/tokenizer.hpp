#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lith {

enum class TokenKind {
    Identifier,
    Number,
    String,
    Keyword,
    Symbol,
    EndOfFile,
    Unknown,
};

struct Token {
    TokenKind kind{};
    std::string lexeme{};
    std::size_t line{1};
    std::size_t column{1};
};

std::vector<Token> tokenize(const std::string& source);
const char* token_kind_name(TokenKind kind);

} // namespace lith
