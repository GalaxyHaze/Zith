#include "lith/tokenizer.hpp"

#include <cctype>
#include <unordered_set>

namespace lith {

namespace {

bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_identifier_part(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_symbol_char(char c) {
    static constexpr const char* symbols = "(){}[],:;.+-*/%<>=!&|^~";
    for (const char* p = symbols; *p != '\0'; ++p) {
        if (*p == c) return true;
    }
    return false;
}

bool is_keyword(const std::string& lexeme) {
    static const std::unordered_set<std::string> keywords = {
        "fn", "let", "var", "if", "else", "for", "return", "struct", "enum", "union"
    };
    return keywords.contains(lexeme);
}

} // namespace

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> out;

    std::size_t i = 0;
    std::size_t line = 1;
    std::size_t col = 1;

    auto push_token = [&](TokenKind kind, std::size_t start, std::size_t end, std::size_t l, std::size_t c) {
        out.push_back(Token{kind, source.substr(start, end - start), l, c});
    };

    while (i < source.size()) {
        const char c = source[i];

        if (c == '\n') {
            ++i;
            ++line;
            col = 1;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            ++col;
            continue;
        }

        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            i += 2;
            col += 2;
            while (i < source.size() && source[i] != '\n') {
                ++i;
                ++col;
            }
            continue;
        }

        const std::size_t start = i;
        const std::size_t tok_line = line;
        const std::size_t tok_col = col;

        if (is_identifier_start(c)) {
            ++i;
            ++col;
            while (i < source.size() && is_identifier_part(source[i])) {
                ++i;
                ++col;
            }
            const std::string lex = source.substr(start, i - start);
            out.push_back(Token{is_keyword(lex) ? TokenKind::Keyword : TokenKind::Identifier, lex, tok_line, tok_col});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            ++i;
            ++col;
            while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) {
                ++i;
                ++col;
            }
            if (i < source.size() && source[i] == '.') {
                ++i;
                ++col;
                while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) {
                    ++i;
                    ++col;
                }
            }
            push_token(TokenKind::Number, start, i, tok_line, tok_col);
            continue;
        }

        if (c == '"') {
            ++i;
            ++col;
            bool escaped = false;
            while (i < source.size()) {
                const char cur = source[i];
                if (!escaped && cur == '"') {
                    ++i;
                    ++col;
                    break;
                }
                if (cur == '\n') {
                    ++line;
                    col = 1;
                    ++i;
                    escaped = false;
                    continue;
                }
                escaped = (!escaped && cur == '\\');
                ++i;
                ++col;
            }
            push_token(TokenKind::String, start, i, tok_line, tok_col);
            continue;
        }

        if (is_symbol_char(c)) {
            ++i;
            ++col;
            push_token(TokenKind::Symbol, start, i, tok_line, tok_col);
            continue;
        }

        ++i;
        ++col;
        push_token(TokenKind::Unknown, start, i, tok_line, tok_col);
    }

    out.push_back(Token{TokenKind::EndOfFile, "", line, col});
    return out;
}

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::Number: return "Number";
        case TokenKind::String: return "String";
        case TokenKind::Keyword: return "Keyword";
        case TokenKind::Symbol: return "Symbol";
        case TokenKind::EndOfFile: return "EOF";
        case TokenKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

} // namespace lith
