#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/lexer/token.hpp"
#include "memory/dyn-array.hpp"
#include "memory/result.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace zith::memory {
struct SourceLoc;
}

namespace zith::lexer {

class Lexer {
    memory::SourceMap &sourceMap;
    // memory::Arena &arena;
    memory::SourceLoc *file = nullptr;
    memory::FileId gId      = 0;
    const char *start       = nullptr;
    const char *now         = nullptr;
    const char *end         = nullptr;

    diagnostics::DiagnosticEngine &diags;
    memory::DynArray<Token> tokens;

    static bool isNum(char c) {
        return c >= '0' && c <= '9';
    }
    static bool isPunctuation(char c);
    static bool isOperator(char c);
    bool isOpen() const {
        return now < end;
    }
    char peek() const {
        return (now + 1 < end) ? now[1] : '\0';
    }
    char peek(size_t n) const {
        return (now + n < end) ? now[n] : '\0';
    }
    bool match(std::string_view must);
    void singleLineComment(bool isDoc);
    void multiLineComment(bool isDoc);
    void processNumber();
    void processString();
    void processIdentifier();

    memory::Span spanAt(const char *p) const noexcept {
        auto off = static_cast<uint32_t>(p - start);
        return {gId, off, off + 1};
    }
    memory::Span spanRange(const char *b, const char *e) const noexcept {
        return {gId, static_cast<uint32_t>(b - start), static_cast<uint32_t>(e - start)};
    }

public:
    Lexer(memory::SourceMap &source_map, memory::Arena &arena,
          diagnostics::DiagnosticEngine &diags);
    auto run(std::variant<memory::FileId, std::pair<std::string_view, std::string>> input)
        -> memory::Result<TokenStream>;
};

[[nodiscard]] auto tokenize(memory::SourceMap &source_map, memory::Arena &arena, memory::FileId id,
                            diagnostics::DiagnosticEngine &diags) -> memory::Result<TokenStream>;

auto tokenize(memory::SourceMap &source_map, memory::Arena &arena, std::string_view, std::string,
              diagnostics::DiagnosticEngine &diags) -> memory::Result<TokenStream>;

const char *tokenKindName(TokenKind k) noexcept;

void printTokens(const TokenStream &stream) noexcept;

} // namespace zith::lexer
