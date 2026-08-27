#include "lexer.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/keyword-table.hpp"
#include "memory/source-file.hpp"
#include "memory/source-map.hpp"
#include "memory/span.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace zith::lexer {

static bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool isAsciiAlnum(char c) {
    return isAsciiAlpha(c) || (c >= '0' && c <= '9');
}

bool Lexer::isPunctuation(char c) {
    switch (c) {
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case ':':
    case ';':
    case ',':
    case '.':
    case '"':
    case '\'':
    case '#':
    case '@':
    case '`':
        return true;
    default:
        return false;
    }
}

bool Lexer::isOperator(char c) {
    switch (c) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '=':
    case '<':
    case '>':
    case '!':
    case '&':
    case '|':
    case '^':
    case '~':
    case '?':
        return true;
    default:
        return false;
    }
}

bool Lexer::match(std::string_view must) {
    if (static_cast<size_t>(end - now) < must.size())
        return false;
    std::string_view rest(now, static_cast<size_t>(end - now));
    if (rest.starts_with(must)) {
        now += must.size();
        return true;
    }
    return false;
}

void Lexer::singleLineComment(bool isDoc = false) {
    const auto before = now - (isDoc ? 3 : 2);
    while (isOpen()) {
        if (*now == '\n') {
            break;
        }
        now++;
    }
    tokens.emplace(spanRange(before, now), (isDoc ? TokenKind::Docs : TokenKind::Comments));
}

void Lexer::multiLineComment(bool isDoc = false) {
    const auto before = now - (isDoc ? 3 : 2);
    const char *delim = (isDoc) ? "/**" : "/*";
    while (isOpen()) {
        if (*now == '*' && peek() == '/') {
            now += 2;
            tokens.emplace(spanRange(before, now), (isDoc ? TokenKind::Docs : TokenKind::Comments));
            return;
        }
        now++;
    }
    diags.report(diagnostics::Severity::Error, diagnostics::err::UnclosedComment,
                 std::string("Unterminated block comment '") + delim + "'", spanRange(before, now));
}

static bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void Lexer::processNumber() {
    const auto before = now;

    if (*now == '0' && now + 1 < end) {
        char nxt = now[1];

        // Hexadecimal
        if (nxt == 'x' || nxt == 'X') {
            now += 2;
            while (now < end && isHexDigit(*now))
                now++;
            if (now == before + 2) {
                diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidIntLiteral,
                             "Invalid hex literal: expected digits after '0x'", spanAt(before));
            }
            tokens.emplace(spanRange(before, now), TokenKind::LitVal);
            return;
        }
        // Octal
        if (nxt == 'c' || nxt == 'C') {
            now += 2;
            while (now < end && *now >= '0' && *now <= '7')
                now++;
            if (now == before + 2) {
                if (now < end && isNum(*now)) {
                    diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidIntLiteral,
                                 "Invalid octal digit in literal", spanAt(now));
                } else {
                    diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidIntLiteral,
                                 "Invalid octal literal: expected digits after '0c'",
                                 spanAt(before));
                }
            } else if (now < end && isNum(*now)) {
                diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidIntLiteral,
                             "Invalid octal digit in literal", spanAt(now));
            }
            tokens.emplace(spanRange(before, now), TokenKind::LitVal);
            return;
        }
        // Binary
        if (nxt == 'b' || nxt == 'B') {
            now += 2;
            while (now < end && (*now == '0' || *now == '1'))
                now++;
            if (now == before + 2) {
                diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidIntLiteral,
                             "Invalid binary literal: expected digits after '0b'", spanAt(before));
            }
            tokens.emplace(spanRange(before, now), TokenKind::LitVal);
            return;
        }
    }

    while (now < end && isNum(*now))
        now++;

    if (now < end && *now == '.' && now + 1 < end && isNum(*(now + 1))) {
        now++;
        while (now < end && isNum(*now))
            now++;
    }

    tokens.emplace(spanRange(before, now), TokenKind::LitVal);
}

void Lexer::processString() {
    const auto before = now;
    const char quote  = *now;
    now++;

    while (isOpen()) {
        if (*now == quote) {
            now++;
            tokens.emplace(spanRange(before, now), TokenKind::LitVal);
            return;
        }
        if (*now == '\\') {
            now++;
            if (!isOpen()) {
                diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                             "Unexpected end of file in escape sequence", spanAt(now - 1));
                tokens.emplace(spanRange(before, now), TokenKind::LitVal);
                return;
            }
            // Line continuation: \ at end of line
            if (*now == '\n' || *now == '\r') {
                if (*now == '\r' && isOpen() && *(now + 1) == '\n')
                    now++;
                now++;
                while (isOpen() && (*now == ' ' || *now == '\t' || *now == '\n' || *now == '\r'))
                    now++;
                continue;
            }
            char esc = *now;
            if (esc != 'n' && esc != 't' && esc != 'r' && esc != '0' && esc != '\\' && esc != '"' &&
                esc != '\'' && esc != '$') {
                diags.report(diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                             std::string("Invalid escape sequence '\\") + esc + "'",
                             spanAt(now - 1));
            }
            now++;
        } else {
            now++;
        }
    }

    diags.report(diagnostics::Severity::Error, diagnostics::err::UnclosedString,
                 "Unterminated string literal", spanRange(before, now));
}

void Lexer::processIdentifier() {
    const auto before = now;
    while (now < end && (isAsciiAlnum(*now) || *now == '_')) {
        now++;
    }
    std::string_view word{before, static_cast<size_t>(now - before)};
    TokenKind kind = lookupKeyword(word);
    tokens.emplace(spanRange(before, now), kind);
}

Lexer::Lexer(memory::SourceMap &source_map, memory::Arena &arena,
             diagnostics::DiagnosticEngine &diags_)
    : sourceMap(source_map), diags(diags_), tokens(arena) {}

auto Lexer::run(std::variant<memory::FileId, std::pair<std::string_view, std::string>> input)
    -> memory::Result<TokenStream> {

    if (auto *id = std::get_if<memory::FileId>(&input)) {
        gId = *id;
    } else {
        auto &[name, content] = std::get<std::pair<std::string_view, std::string>>(input);
        auto add_result       = sourceMap.addFile(name, content);
        if (!add_result)
            return memory::Error{add_result.error().msg};
        gId = add_result.value();
    }

    if (auto i = sourceMap.get(gId)) {
        file = &i.value().get();
    }

    if (!file)
        return memory::Error{"[error] Couldnt find the file"};

    const auto content = file->getSlice();
    start = now = content.data();
    end         = content.data() + content.size();

    while (isOpen()) {
        if (isAsciiSpace(*now)) {
            now++;
            continue;
        }

        if (match("///")) {
            singleLineComment(true);
            continue;
        }

        if (match("//")) {
            singleLineComment();
            continue;
        }

        if (match("/**")) {
            multiLineComment(true);
            continue;
        }

        if (match("/*")) {
            multiLineComment();
            continue;
        }

        if (*now == '"' || *now == '\'') {
            processString();
            continue;
        }

        if (isNum(*now)) {
            processNumber();
            continue;
        }

        if (isAsciiAlpha(*now) || *now == '_') {
            processIdentifier();
            continue;
        }

        if (isOperator(*now)) {
            const auto before = now;
            now++;
            tokens.emplace(spanRange(before, now), TokenKind::Operators, *before);
            continue;
        }

        if (isPunctuation(*now)) {
            const auto before = now;
            now++;
            tokens.emplace(spanRange(before, now), TokenKind::Punctuation, *before);
            continue;
        }

        {
            const auto before = now;
            now++;
            auto span = spanRange(before, now);
            diags.report(diagnostics::Severity::Error, diagnostics::err::UnknownToken,
                         std::string("Unexpected character '") + *before + "'", span);
            tokens.emplace(span, TokenKind::Unknown);
        }
    }

    tokens.emplace(spanRange(now, now), TokenKind::End);

    if (diags.hasErrors())
        return memory::Error{"tokenization failed — see diagnostics for details"};

    return TokenStream{tokens.data(), static_cast<uint32_t>(tokens.size()), 0, start};
}

auto tokenize(memory::SourceMap &source_map, memory::Arena &arena, memory::FileId id,
              diagnostics::DiagnosticEngine &diags) -> memory::Result<TokenStream> {
    Lexer lexer(source_map, arena, diags);
    return lexer.run(std::variant<memory::FileId, std::pair<std::string_view, std::string>>(id));
}

auto tokenize(memory::SourceMap &source_map, memory::Arena &arena, std::string_view name,
              std::string content, diagnostics::DiagnosticEngine &diags)
    -> memory::Result<TokenStream> {
    Lexer lexer(source_map, arena, diags);
    return lexer.run(std::make_pair(name, std::move(content)));
}

const char *tokenKindName(TokenKind k) noexcept {
    static constexpr const char *names[] = {
        "Identifier", "As",       "Using",     "Type",       "Struct",      "Raw",     "Must",
        "Mutable",    "Trait",    "Interface", "Typedef",    "Implement",   "Fn",      "Module",
        "Extern",     "Macro",    "Context",   "Variable",   "Ownership",   "Yield",   "Label",
        "Visibility", "If",       "For",       "In",         "Match",       "Control", "Thread",
        "Error",      "Drop",     "Require",   "Is",         "Word",        "Logical", "Comparison",
        "Operators",  "Comments", "Docs",      "Annotation", "Punctuation", "LitVal",  "Unknown",
        "End"};
    static_assert(std::size(names) == static_cast<size_t>(TokenKind::End) + 1,
                  "tokenKindName array must match TokenKind enum");
    size_t idx = static_cast<size_t>(k);
    if (idx < std::size(names))
        return names[idx];
    return "???";
}

void printTokens(const TokenStream &stream) noexcept {
    for (uint32_t i = 0; i < stream.len; ++i) {
        const auto &tok = stream.src[i];
        auto lexeme     = stream.lexeme(tok);
        printf("  %-16s \"%.*s\"  [%u..%u]\n", tokenKindName(tok.kind), (int)lexeme.size(),
               lexeme.data(), tok.span.start, tok.span.end);
    }
}

} // namespace zith::lexer
