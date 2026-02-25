// impl/parse/tokenizer.cpp
#include "Nova/nova.h"
#include <string_view>
#include <vector>
#include <cstring>
#include <iostream>

#define NOVA_NEWLINE(loc) do { \
    (loc)->line++;             \
    (loc)->index = 0;          \
} while(0)

namespace nova::detail {

struct LexError {
    const char*   msg;
    NovaSourceLoc info;
};

// ── Helpers de arena ──────────────────────────────────────────────────────────

static void uint_to_str(char* buf, size_t buf_size, uint64_t value) {
    if (buf_size == 0) return;
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[21];
    size_t len = 0;
    while (value > 0 && len < sizeof(temp) - 1) {
        temp[len++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    size_t out_len = (len < buf_size) ? len : buf_size - 1;
    for (size_t i = 0; i < out_len; ++i)
        buf[i] = temp[len - 1 - i];
    buf[out_len] = '\0';
}

static const char* make_error_msg(NovaArena* arena, const char* prefix, uint64_t line) {
    char stack_buf[128];
    size_t pos = 0;
    for (const char* p = prefix; *p && pos < sizeof(stack_buf) - 22;)
        stack_buf[pos++] = *p++;
    uint_to_str(stack_buf + pos, sizeof(stack_buf) - pos, line);
    size_t total_len = strlen(stack_buf);
    char* msg = static_cast<char*>(nova_arena_alloc(arena, total_len + 1));
    if (msg) std::memcpy(msg, stack_buf, total_len + 1);
    return msg;
}

static NovaToken make_token(NovaArena* arena, NovaTokenType type,
                            std::string_view lexeme, NovaSourceLoc info) {
    auto* buf = static_cast<char*>(nova_arena_alloc(arena, lexeme.size()));
    if (buf && !lexeme.empty())
        std::memcpy(buf, lexeme.data(), lexeme.size());
    return NovaToken{
        .lexeme     = {buf, lexeme.size()},
        .loc        = info,
        .type       = type,
        .keyword_id = 0
    };
}

// ── Char classifiers ──────────────────────────────────────────────────────────

static bool isSpace   (unsigned char c) { return ::isspace(c)  != 0; }
static bool isAlpha   (unsigned char c) { return ::isalpha(c)  != 0; }
static bool isDigit   (unsigned char c) { return ::isdigit(c)  != 0; }
static bool isAlphaNum(unsigned char c) { return ::isalnum(c)  != 0; }
static bool isHexDigit(unsigned char c) { return ::isxdigit(c) != 0; }
static unsigned char toLower(unsigned char c) { return static_cast<unsigned char>(::tolower(c)); }

static bool isValidEscape(char c) {
    switch (c) {
        case 'n': case 't': case 'r':
        case '\\': case '"': case '0':
            return true;
        default:
            return false;
    }
}

// ── Forward declarations ──────────────────────────────────────────────────────

static void processIdentifier(const char*& current, const char* end,
                              std::vector<NovaToken>& tokens,
                              NovaSourceLoc& info, NovaArena* arena);

static void processString(const char*& current, const char* end,
                          std::vector<NovaToken>& tokens, std::vector<LexError>& errors,
                          NovaSourceLoc& info, NovaArena* arena);

static void processNumber(const char*& current, const char* end,
                          std::vector<NovaToken>& tokens, std::vector<LexError>& errors,
                          NovaSourceLoc& info, NovaArena* arena);

static bool punctuation(const char*& current, const char* end,
                        std::vector<NovaToken>& tokens,
                        NovaSourceLoc& info, NovaArena* arena);

// ── Comentários ───────────────────────────────────────────────────────────────

static void skipSingleLine(NovaSourceLoc& info, const char*& current, const char* end) {
    while (current < end && *current != '\n') {
        ++current; ++info.index;
    }
}

static void skipMultiLine(NovaSourceLoc& info, const char*& current, const char* end,
                          std::vector<LexError>& errors, NovaArena* arena) {
    const auto start = info;
    current += 2; info.index += 2;

    while (current < end) {
        if (*current == '*' && current + 1 < end && *(current + 1) == '/') {
            current += 2; info.index += 2;
            return;
        }
        if (*current == '\n') NOVA_NEWLINE(&info);
        ++current; ++info.index;
    }
    errors.push_back({
        make_error_msg(arena, "Unterminated multi-line comment starting at line ", start.line),
        start
    });
}

// ── Erros ─────────────────────────────────────────────────────────────────────

static void addCharError(std::vector<LexError>& errors, const char* base_msg,
                         const char c, const NovaSourceLoc info, NovaArena* arena) {
    char stack_buf[64];
    size_t pos = 0;
    for (const char* p = base_msg; *p && pos < sizeof(stack_buf) - 5;)
        stack_buf[pos++] = *p++;
    stack_buf[pos++] = '\'';
    stack_buf[pos++] = c;
    stack_buf[pos++] = '\'';
    stack_buf[pos]   = '\0';
    errors.push_back({ make_error_msg(arena, stack_buf, info.line), info });
}

static void addMsgError(std::vector<LexError>& errors, NovaArena* arena,
                        const char* msg, const NovaSourceLoc info) {
    const size_t len = strlen(msg);
    char* copy = static_cast<char*>(nova_arena_alloc(arena, len + 1));
    if (copy) std::memcpy(copy, msg, len + 1);
    errors.push_back({ copy, info });
}

// ── Processadores ─────────────────────────────────────────────────────────────

static void processIdentifier(const char*& current, const char* end,
                              std::vector<NovaToken>& tokens,
                              NovaSourceLoc& info, NovaArena* arena) {
    const char* start = current;
    while (current < end && (isAlphaNum(static_cast<unsigned char>(*current)) || *current == '_')) {
        ++current; ++info.index;
    }
    const std::string_view lexeme(start, current - start);
    const NovaTokenType type = nova_lookup_keyword(start, current - start);
    tokens.push_back(make_token(arena, type, lexeme, info));
}

static void processString(const char*& current, const char* end,
                          std::vector<NovaToken>& tokens, std::vector<LexError>& errors,
                          NovaSourceLoc& info, NovaArena* arena) {
    const NovaSourceLoc startInfo = info;
    const char* start = current;
    ++current; ++info.index; // consome a aspa de abertura

    while (current < end) {
        if (*current == '"') {
            ++current; ++info.index;
            tokens.push_back(make_token(arena, NOVA_TOKEN_STRING,
                                        std::string_view(start, current - start), startInfo));
            return;
        }

        if (*current == '\\') {
            const NovaSourceLoc escapeLoc = info;
            ++current; ++info.index;

            if (current >= end) {
                addMsgError(errors, arena,
                    "Unterminated escape sequence at end of file", escapeLoc);
                break;
            }

            if (!isValidEscape(*current)) {
                // monta "Invalid escape sequence '\X' at line N"
                char msg_buf[64] = "Invalid escape sequence '\\";
                size_t pos = strlen(msg_buf);
                if (pos < sizeof(msg_buf) - 2) {
                    msg_buf[pos++] = *current;
                    msg_buf[pos++] = '\'';
                    msg_buf[pos]   = '\0';
                }
                errors.push_back({
                    make_error_msg(arena, msg_buf, escapeLoc.line),
                    escapeLoc
                });
                // recuperação: consome o caractere inválido e continua
            }
            ++current; ++info.index;
            continue;
        }

        // newline dentro de string é permitido
        if (*current == '\n') NOVA_NEWLINE(&info);
        ++current; ++info.index;
    }

    // chegou ao fim sem fechar a string
    errors.push_back({
        make_error_msg(arena, "Unterminated string literal starting at line ", startInfo.line),
        startInfo
    });
    tokens.push_back(make_token(arena, NOVA_TOKEN_STRING,
                                std::string_view(start, current - start), startInfo));
}

static void processNumber(const char*& current, const char* end,
                          std::vector<NovaToken>& tokens, std::vector<LexError>& errors,
                          NovaSourceLoc& info, NovaArena* arena) {
    const char*           start     = current;
    const NovaSourceLoc   startInfo = info;

    enum class Base { Decimal, Hex, Binary, Octal } base = Base::Decimal;

    // Detecta prefixo de base
    if (*current == '0' && current + 1 < end) {
        if (const char next = static_cast<char>(toLower(static_cast<unsigned char>(*(current + 1)))); next == 'x') {
            base = Base::Hex;
            current += 2; info.index += 2;
            if (current >= end || !isHexDigit(static_cast<unsigned char>(*current))) {
                errors.push_back({
                    make_error_msg(arena, "Hex literal '0x' has no digits at line ", startInfo.line),
                    startInfo
                });
                tokens.push_back(make_token(arena, NOVA_TOKEN_HEXADECIMAL,
                                            std::string_view(start, current - start), startInfo));
                return;
            }
        } else if (next == 'b') {
            base = Base::Binary;
            current += 2; info.index += 2;
            if (current >= end || (*current != '0' && *current != '1')) {
                errors.push_back({
                    make_error_msg(arena, "Binary literal '0b' has no digits at line ", startInfo.line),
                    startInfo
                });
                tokens.push_back(make_token(arena, NOVA_TOKEN_BINARY,
                                            std::string_view(start, current - start), startInfo));
                return;
            }
        } else if (next == 'o') {
            base = Base::Octal;
            current += 2; info.index += 2;
            if (current >= end || *current < '0' || *current > '7') {
                errors.push_back({
                    make_error_msg(arena, "Octal literal '0o' has no digits at line ", startInfo.line),
                    startInfo
                });
                tokens.push_back(make_token(arena, NOVA_TOKEN_OCTAL,
                                            std::string_view(start, current - start), startInfo));
                return;
            }
        }
    }

    bool isFloat = false;

    while (current < end) {
        const auto c = static_cast<unsigned char>(*current);

        if (c == '_') { ++current; ++info.index; continue; } // separador visual: 1_000_000

        switch (base) {
            case Base::Hex:
                if (!isHexDigit(c)) goto done;
                break;
            case Base::Binary:
                if (c != '0' && c != '1') goto done;
                break;
            case Base::Octal:
                if (c < '0' || c > '7') goto done;
                // dígito octal válido mas seguido de 8 ou 9 é erro de digitação comum
                if (current + 1 < end) {
                    const unsigned char next = static_cast<unsigned char>(*(current + 1));
                    if (next == '8' || next == '9') {
                        ++current; ++info.index; // consome o dígito atual
                        errors.push_back({
                            make_error_msg(arena, "Invalid digit in octal literal at line ", info.line),
                            info
                        });
                        // consome o inválido e segue para done
                        ++current; ++info.index;
                        goto done;
                    }
                }
                break;
            case Base::Decimal:
                if (c == '.') {
                    if (isFloat) goto done; // segundo ponto — para; o Parser verá DOT
                    if (!(current + 1 < end && isDigit(static_cast<unsigned char>(*(current + 1)))))
                        goto done; // ponto não seguido de dígito — não faz parte do literal
                    isFloat = true;
                } else if (!isDigit(c)) {
                    goto done;
                }
                break;
        }
        ++current; ++info.index;
    }

done:
    // Sufixo alfanumérico colado no literal é sempre erro: 123abc, 0xFFgg, 0b101zz
    if (current < end && (isAlpha(static_cast<unsigned char>(*current)) || *current == '_')) {
        const char*         suffixStart = current;
        const NovaSourceLoc suffixLoc   = info;

        while (current < end && (isAlphaNum(static_cast<unsigned char>(*current)) || *current == '_')) {
            ++current; ++info.index;
        }

        char msg_buf[160];
        size_t pos = 0;
        const char* pfx = "Invalid suffix '";
        for (const char* p = pfx; *p && pos < sizeof(msg_buf) - 1;) msg_buf[pos++] = *p++;
        for (const char* p = suffixStart; p < current && pos < sizeof(msg_buf) - 1;) msg_buf[pos++] = *p++;
        const char* sfx = "' on numeric literal at line ";
        for (const char* p = sfx; *p && pos < sizeof(msg_buf) - 1;) msg_buf[pos++] = *p++;
        msg_buf[pos] = '\0';

        errors.push_back({
            make_error_msg(arena, msg_buf, suffixLoc.line),
            suffixLoc
        });
    }

    NovaTokenType type = NOVA_TOKEN_NUMBER;
    switch (base) {
        case Base::Hex:     type = NOVA_TOKEN_HEXADECIMAL;                       break;
        case Base::Binary:  type = NOVA_TOKEN_BINARY;                            break;
        case Base::Octal:   type = NOVA_TOKEN_OCTAL;                             break;
        case Base::Decimal: type = isFloat ? NOVA_TOKEN_FLOAT : NOVA_TOKEN_NUMBER; break;
    }

    tokens.push_back(make_token(arena, type,
                                std::string_view(start, current - start), startInfo));
}

static bool punctuation(const char*& current, const char* end,
                        std::vector<NovaToken>& tokens,
                        NovaSourceLoc& info, NovaArena* arena) {
    for (const int len : {3, 2, 1}) {
        if (current + len > end) continue;
        const auto t = nova_lookup_keyword(current, static_cast<size_t>(len));
        if (t != NOVA_TOKEN_IDENTIFIER) {
            const std::string_view view(current, static_cast<size_t>(len));
            current    += len;
            info.index += static_cast<size_t>(len);
            tokens.push_back(make_token(arena, t, view, info));
            return true;
        }
    }
    return false;
}

// ── Loop principal ────────────────────────────────────────────────────────────

static std::vector<NovaToken> tokenize(std::string_view src, NovaArena* arena,
                                       std::vector<LexError>& errors) {
    std::vector<NovaToken> tokens;
    tokens.reserve(src.size() / 2 + 1);

    NovaSourceLoc info{0, 1};
    const char* current = src.data();
    const char* end     = src.data() + src.size();

    while (current < end) {
        const auto c = static_cast<unsigned char>(*current);

        if (isSpace(c)) {
            if (*current == '\n') NOVA_NEWLINE(&info);
            ++current; ++info.index;
            continue;
        }

        if (*current == '/' && current + 1 < end) {
            if (*(current + 1) == '/') { skipSingleLine(info, current, end); continue; }
            if (*(current + 1) == '*') { skipMultiLine(info, current, end, errors, arena); continue; }
        }

        if (isAlpha(c) || *current == '_') {
            processIdentifier(current, end, tokens, info, arena);
            continue;
        }

        if (isDigit(c) || (*current == '.' && current + 1 < end &&
                            isDigit(static_cast<unsigned char>(*(current + 1))))) {
            processNumber(current, end, tokens, errors, info, arena);
            continue;
        }

        if (*current == '"') {
            processString(current, end, tokens, errors, info, arena);
            continue;
        }

        if (punctuation(current, end, tokens, info, arena))
            continue;

        addCharError(errors, "Unknown character ", *current, info, arena);
        tokens.push_back(make_token(arena, NOVA_TOKEN_UNKNOWN,
                                    std::string_view(current, 1), info));
        ++current; ++info.index;
    }

    tokens.push_back(make_token(arena, NOVA_TOKEN_END, std::string_view{}, info));
    return tokens;
}

} // namespace nova::detail

// ── C API ─────────────────────────────────────────────────────────────────────

NovaTokenStream nova_tokenize(NovaArena* arena, const char* source, const size_t source_len) {
    if (!arena || !source ) return {nullptr, 0};

    std::vector<nova::detail::LexError> errors;
    const auto tokens = nova::detail::tokenize(
        std::string_view(source, source_len), arena, errors);

    if (!errors.empty()) {
        for (const auto& [msg, info] : errors)
            std::cerr << "Lexical error (line " << info.line
                      << ", col " << info.index << "): " << msg << '\n';
        return {nullptr, 0};
    }

    const size_t total_size = sizeof(NovaToken) * tokens.size();
    auto* buf = static_cast<NovaToken*>(nova_arena_alloc(arena, total_size));
    if (!buf) return {nullptr, 0};

    return {buf, total_size};
}

#undef NOVA_NEWLINE