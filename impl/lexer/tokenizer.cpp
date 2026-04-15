// impl/parser/tokenizer.cpp
#include "zith/zith.hpp"
#include "../memory/utils.h"
#include <string_view>
#include <vector>
#include <cstring>
#include <iostream>

#define ZITH_NEWLINE(loc) do { \
    (loc)->line++;             \
    (loc)->index = 0;          \
} while(0)

#define MAX_ERRORS 50

namespace zith::detail {
    struct LexError {
        const char *msg;
        ZithSourceLoc info;
    };

    // Tipo específico para o Tokenizer
    using TokenList = ArenaList<ZithToken>;


    // ============================================================================
    // Helpers
    // ============================================================================

    static void uint_to_str(char *buf, const size_t buf_size, uint64_t value, size_t *out_len) {
        if (buf_size == 0) return;
        if (value == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            *out_len = 1;
            return;
        }
        char temp[21];
        size_t len = 0;
        while (value > 0 && len < sizeof(temp) - 1) {
            temp[len++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        const size_t out_len_val = (len < buf_size) ? len : buf_size - 1;
        for (size_t i = 0; i < out_len_val; ++i)
            buf[i] = temp[len - 1 - i];
        buf[out_len_val] = '\0';
        *out_len = out_len_val;
    }

    static const char *make_error_msg(ZithArena *arena, const char *prefix, const uint64_t line) {
        char stack_buf[128];
        size_t pos = 0;
        for (const char *p = prefix; *p && pos < sizeof(stack_buf) - 22;)
            stack_buf[pos++] = *p++;

        size_t num_len = 0;
        uint_to_str(stack_buf + pos, sizeof(stack_buf) - pos, line, &num_len);
        pos += num_len;

        char *msg = static_cast<char *>(zith_arena_alloc(arena, pos + 1));
        if (msg) std::memcpy(msg, stack_buf, pos + 1);
        return msg;
    }

    static ZithToken make_token(ZithArena *arena, ZithTokenType type,
                                    std::string_view lexeme, ZithSourceLoc info) {
        auto *buf = static_cast<char *>(zith_arena_alloc(arena, lexeme.size()));
        if (buf && !lexeme.empty())
            std::memcpy(buf, lexeme.data(), lexeme.size());
        return ZithToken{
            .lexeme = {buf, lexeme.size()},
            .loc = info,
            .type = type,
            .keyword_id = 0
        };
    }

    // ── Char classifiers ─────────────────────────────────────────────────────────

    static bool isSpace(unsigned char c) { return ::isspace(c) != 0; }
    static bool isAlpha(unsigned char c) { return ::isalpha(c) != 0; }
    static bool isDigit(unsigned char c) { return ::isdigit(c) != 0; }
    static bool isAlphaNum(unsigned char c) { return ::isalnum(c) != 0; }
    static bool isHexDigit(unsigned char c) { return ::isxdigit(c) != 0; }
    static unsigned char toLower(unsigned char c) { return static_cast<unsigned char>(::tolower(c)); }

    static bool isValidEscape(char c) {
        switch (c) {
            case 'n':
            case 't':
            case 'r':
            case '\\':
            case '"':
            case '0':
            case '\'':
                return true;
            default:
                return false;
        }
    }

    // ── Forward declarations ─────────────────────────────────────────────────────

    static void processIdentifier(const char *&current, const char *end,
                                  TokenList &tokens,
                                  ZithSourceLoc &info, ZithArena *arena);

    static void processString(const char *&current, const char *end,
                              TokenList &tokens, std::vector<LexError> &error_list,
                              ZithSourceLoc &info, ZithArena *arena);

    static void processNumber(const char *&current, const char *end,
                              TokenList &tokens, std::vector<LexError> &error_list,
                              ZithSourceLoc &info, ZithArena *arena);

    static bool punctuation(const char *&current, const char *end,
                            TokenList &tokens,
                            ZithSourceLoc &info, ZithArena *arena);

    // ── Comments ─────────────────────────────────────────────────────────────────

    static void skipSingleLine(ZithSourceLoc &info, const char *&current, const char *end) {
        while (current < end && *current != '\n') {
            ++current;
            ++info.index;
        }
    }

    static void skipMultiLine(ZithSourceLoc &info, const char *&current, const char *end,
                              std::vector<LexError> &error_list, ZithArena *arena) {
        const auto start = info;
        current += 2;
        info.index += 2;

        while (current < end) {
            if (*current == '*' && current + 1 < end && *(current + 1) == '/') {
                current += 2;
                info.index += 2;
                return;
            }
            if (*current == '\n')
                ZITH_NEWLINE(&info);
            ++current;
            ++info.index;
        }
        error_list.push_back({
            make_error_msg(arena, "Unterminated multi-line comment starting at line ", start.line),
            start
        });
    }

    // ── Errors ───────────────────────────────────────────────────────────────────

    static void addCharError(std::vector<LexError> &error_list, const char *base_msg,
                             const char c, const ZithSourceLoc info, ZithArena *arena) {
        if (error_list.size() >= MAX_ERRORS) return;

        char stack_buf[64];
        size_t pos = 0;
        for (const char *p = base_msg; *p && pos < sizeof(stack_buf) - 5;)
            stack_buf[pos++] = *p++;
        stack_buf[pos++] = '\'';
        stack_buf[pos++] = c;
        stack_buf[pos++] = '\'';
        stack_buf[pos] = '\0';
        error_list.push_back({make_error_msg(arena, stack_buf, info.line), info});
    }

    static void addMsgError(std::vector<LexError> &error_list, ZithArena *arena,
                            const char *msg, const ZithSourceLoc info) {
        if (error_list.size() >= MAX_ERRORS) return;

        const size_t len = strlen(msg);
        char *copy = static_cast<char *>(zith_arena_alloc(arena, len + 1));
        if (copy) std::memcpy(copy, msg, len + 1);
        error_list.push_back({copy, info});
    }

    // ── Processors ──────────────────────────────────────────────────────────────

    static void processIdentifier(const char *&current, const char *end,
                                  TokenList &tokens,
                                  ZithSourceLoc &info, ZithArena *arena) {
        const ZithSourceLoc startInfo = info;
        const char *start = current;

        while (current < end && (isAlphaNum(static_cast<unsigned char>(*current)) || *current == '_')) {
            ++current;
            ++info.index;
        }
        const std::string_view lexeme(start, current - start);
        const ZithTokenType type = zith_lookup_keyword(start, current - start);
        tokens.push(arena, make_token(arena, type, lexeme, startInfo));
    }

    static void processString(const char *&current, const char *end,
                              TokenList &tokens, std::vector<LexError> &error_list,
                              ZithSourceLoc &info, ZithArena *arena) {
        const ZithSourceLoc startInfo = info;
        const char *start = current;
        ++current;
        ++info.index;

        while (current < end) {
            if (*current == '"') {
                ++current;
                ++info.index;
                tokens.push(arena, make_token(arena, ZITH_TOKEN_STRING,
                                              std::string_view(start, current - start), startInfo));
                return;
            }

            if (*current == '\\') {
                const ZithSourceLoc escapeLoc = info;
                ++current;
                ++info.index;

                if (current >= end) {
                    addMsgError(error_list, arena,
                                "Unterminated escape sequence at end of file", escapeLoc);
                    // Fall through to emit error token below
                    break;
                }

                if (!isValidEscape(*current)) {
                    char msg_buf[64];
                    size_t pos = 0;
                    const char *pfx = "Invalid escape sequence '\\";
                    for (const char *p = pfx; *p && pos < sizeof(msg_buf) - 2;) msg_buf[pos++] = *p++;
                    if (pos < sizeof(msg_buf) - 1) msg_buf[pos++] = *current;
                    if (pos < sizeof(msg_buf) - 1) msg_buf[pos++] = '\'';
                    msg_buf[pos] = '\0';
                    error_list.push_back({
                        make_error_msg(arena, msg_buf, escapeLoc.line),
                        escapeLoc
                    });
                }
                ++current;
                ++info.index;
                continue;
            }

            if (*current == '\n')
                ZITH_NEWLINE(&info);
            ++current;
            ++info.index;
        }

        // Only reached on unterminated string (EOF without closing '"')
        error_list.push_back({
            make_error_msg(arena, "Unterminated string literal starting at line ", startInfo.line),
            startInfo
        });
        tokens.push(arena, make_token(arena, ZITH_TOKEN_STRING,
                                      std::string_view(start, current - start), startInfo));
    }

    static void processNumber(const char *&current, const char *end,
                              TokenList &tokens, std::vector<LexError> &error_list,
                              ZithSourceLoc &info, ZithArena *arena) {
        const char *start = current;
        const ZithSourceLoc startInfo = info;

        enum class Base { Decimal, Hex, Binary, Octal } base = Base::Decimal;

        // ── Detect base prefix ────────────────────────────────────────────────────
        if (*current == '0' && current + 1 < end) {
            const char next = static_cast<char>(toLower(static_cast<unsigned char>(*(current + 1))));

            if (next == 'x') {
                base = Base::Hex;
                current += 2;
                info.index += 2;
                if (current >= end || !isHexDigit(static_cast<unsigned char>(*current))) {
                    error_list.push_back({
                        make_error_msg(arena, "Hex literal '0x' has no digits at line ", startInfo.line),
                        startInfo
                    });
                    tokens.push(arena, make_token(arena, ZITH_TOKEN_HEXADECIMAL,
                                                  std::string_view(start, current - start), startInfo));
                    return;
                }
            } else if (next == 'b') {
                base = Base::Binary;
                current += 2;
                info.index += 2;
                if (current >= end || (*current != '0' && *current != '1')) {
                    error_list.push_back({
                        make_error_msg(arena, "Binary literal '0b' has no digits at line ", startInfo.line),
                        startInfo
                    });
                    tokens.push(arena, make_token(arena, ZITH_TOKEN_BINARY,
                                                  std::string_view(start, current - start), startInfo));
                    return;
                }
            } else if (next == 'o') {
                base = Base::Octal;
                current += 2;
                info.index += 2;
                if (current >= end || *current < '0' || *current > '7') {
                    error_list.push_back({
                        make_error_msg(arena, "Octal literal '0o' has no digits at line ", startInfo.line),
                        startInfo
                    });
                    tokens.push(arena, make_token(arena, ZITH_TOKEN_OCTAL,
                                                  std::string_view(start, current - start), startInfo));
                    return;
                }
            }
        }

        bool isFloat = false;
        bool prev_is_separator = false;

        while (current < end) {
            const auto c = static_cast<unsigned char>(*current);

            if (c == '\'') {
                if (prev_is_separator) {
                    error_list.push_back({
                        make_error_msg(arena, "Consecutive separators in numeric literal at line ", info.line),
                        info
                    });
                }
                prev_is_separator = true;
                ++current;
                ++info.index;
                continue;
            }

            switch (base) {
                case Base::Hex:
                    if (!isHexDigit(c)) goto done;
                    break;

                case Base::Binary:
                    if (c != '0' && c != '1') goto done;
                    break;

                case Base::Octal:
                    if (c < '0' || c > '7') {
                        // FIX: detect 8/9 as invalid octal digit (single advance, single error)
                        if (c == '8' || c == '9') {
                            error_list.push_back({
                                make_error_msg(arena, "Invalid digit in octal literal at line ", info.line),
                                info
                            });
                            ++current;
                            ++info.index;
                        }
                        goto done;
                    }
                    break;

                case Base::Decimal:
                    if (c == '.') {
                        if (isFloat) goto done;
                        if (!(current + 1 < end && isDigit(static_cast<unsigned char>(*(current + 1)))))
                            goto done;
                        isFloat = true;
                    } else if (!isDigit(c)) {
                        goto done;
                    }
                    break;
            }

            prev_is_separator = false;
            ++current;
            ++info.index;
        }

    done:
        if (prev_is_separator) {
            error_list.push_back({
                make_error_msg(arena, "Trailing separator in numeric literal at line ", info.line),
                info
            });
        }

        // Detect invalid suffix (e.g. 123abc)
        if (current < end && (isAlpha(static_cast<unsigned char>(*current)) || *current == '_')) {
            const char *suffixStart = current;
            const ZithSourceLoc suffixLoc = info;

            while (current < end && (isAlphaNum(static_cast<unsigned char>(*current)) || *current == '_')) {
                ++current;
                ++info.index;
            }

            char msg_buf[160];
            size_t pos = 0;
            for (const char *p = "Invalid suffix '"; *p && pos < sizeof(msg_buf) - 1;) msg_buf[pos++] = *p++;
            for (const char *p = suffixStart; p < current && pos < sizeof(msg_buf) - 1;) msg_buf[pos++] = *p++;
            for (const char *p = "' on numeric literal at line "; *p && pos < sizeof(msg_buf) - 1;)
                msg_buf[pos++] = *p++;
            msg_buf[pos] = '\0';

            error_list.push_back({
                make_error_msg(arena, msg_buf, suffixLoc.line),
                suffixLoc
            });
        }

        ZithTokenType type = ZITH_TOKEN_NUMBER;
        switch (base) {
            case Base::Hex: type = ZITH_TOKEN_HEXADECIMAL;
                break;
            case Base::Binary: type = ZITH_TOKEN_BINARY;
                break;
            case Base::Octal: type = ZITH_TOKEN_OCTAL;
                break;
            case Base::Decimal: type = isFloat ? ZITH_TOKEN_FLOAT : ZITH_TOKEN_NUMBER;
                break;
        }

        tokens.push(arena, make_token(arena, type,
                                      std::string_view(start, current - start), startInfo));
    }

    static bool punctuation(const char *&current, const char *end,
                            TokenList &tokens,
                            ZithSourceLoc &info, ZithArena *arena) {
        const char c = *current;

        // Single-character tokens — no lookahead needed
        switch (c) {
            case '(':
            case ')':
            case '{':
            case '}':
            case '[':
            case ']':
            case ';':
            case ',':
            case ':':
            case '?':
            case '@':
            case '#':
            case '~':
                tokens.push(arena, make_token(arena, zith_lookup_keyword(&c, 1),
                                              std::string_view(&c, 1), info));
                ++current;
                ++info.index;
                return true;

            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '^':
            case '&':
            case '|':
            case '=':
            case '!':
            case '<':
            case '>':
            case '.':
                break;

            default:
                return false;
        }

        // FIX: capture loc *before* advancing so the stored position is correct
        const ZithSourceLoc startInfo = info;

        for (const int len: {3, 2, 1}) {
            if (current + len > end) continue;
            const auto t = zith_lookup_keyword(current, static_cast<size_t>(len));
            if (t != ZITH_TOKEN_IDENTIFIER) {
                const std::string_view view(current, static_cast<size_t>(len));
                current += len;
                info.index += static_cast<size_t>(len);
                tokens.push(arena, make_token(arena, t, view, startInfo)); // use startInfo, not info
                return true;
            }
        }
        return false;
    }

    // ── Main Loop ───────────────────────────────────────────────────────────────

    static void tokenize(std::string_view src, ZithArena *arena,
                         TokenList &tokens, std::vector<LexError> &error_list) {
        tokens.init(arena, 64);

        ZithSourceLoc info{0, 1};
        const char *current = src.data();
        const char *end = src.data() + src.size();

        while (current < end) {
            const auto c = static_cast<unsigned char>(*current);

            if (isSpace(c)) {
                if (*current == '\n')
                    ZITH_NEWLINE(&info);
                ++current;
                ++info.index;
                continue;
            }

            if (*current == '/' && current + 1 < end) {
                if (*(current + 1) == '/') {
                    skipSingleLine(info, current, end);
                    continue;
                }
                if (*(current + 1) == '*') {
                    skipMultiLine(info, current, end, error_list, arena);
                    continue;
                }
            }

            if (isAlpha(c) || *current == '_') {
                processIdentifier(current, end, tokens, info, arena);
                continue;
            }

            if (isDigit(c) || (*current == '.' && current + 1 < end &&
                               isDigit(static_cast<unsigned char>(*(current + 1))))) {
                processNumber(current, end, tokens, error_list, info, arena);
                continue;
            }

            if (*current == '"') {
                processString(current, end, tokens, error_list, info, arena);
                continue;
            }

            if (punctuation(current, end, tokens, info, arena))
                continue;

            addCharError(error_list, "Unknown character ", *current, info, arena);
            tokens.push(arena, make_token(arena, ZITH_TOKEN_UNKNOWN,
                                          std::string_view(current, 1), info));
            ++current;
            ++info.index;

            if (error_list.size() >= MAX_ERRORS) break;
        }

        tokens.push(arena, make_token(arena, ZITH_TOKEN_END, std::string_view{}, info));
    }
} // namespace zith::detail


// ============================================================================
// C API
// ============================================================================

ZithTokenStream zith_tokenize(ZithArena *arena, const char *source, const size_t source_len) {
    if (!arena || !source) return {nullptr, 0};

    std::vector<zith::detail::LexError> error_list;
    zith::detail::TokenList tokens;

    zith::detail::tokenize(std::string_view(source, source_len), arena, tokens, error_list);

    if (!error_list.empty()) {
        for (const auto &err: error_list)
            std::cerr << "Lexical error (line " << err.info.line
                    << ", col " << err.info.index << "): " << err.msg << '\n';
        return {nullptr, 0};
    }

    size_t count = 0;
    ZithToken *flat_data = tokens.flatten(arena, &count);

    return {flat_data, count};
}


// ============================================================================
// Debug
// ============================================================================

static const char *token_type_name(ZithTokenType type) {
    switch (type) {
        case ZITH_TOKEN_IDENTIFIER: return "IDENTIFIER";
        case ZITH_TOKEN_STRING: return "STRING";
        case ZITH_TOKEN_NUMBER: return "NUMBER";
        case ZITH_TOKEN_FLOAT: return "FLOAT";
        case ZITH_TOKEN_HEXADECIMAL: return "HEX";
        case ZITH_TOKEN_BINARY: return "BINARY";
        case ZITH_TOKEN_OCTAL: return "OCTAL";
        case ZITH_TOKEN_UNKNOWN: return "UNKNOWN";
        case ZITH_TOKEN_END: return "END";
        default: return "KEYWORD/OP";
    }
}

void zith_debug_tokens(ZithArena *arena, const char *source, const size_t source_len) {
    if (!arena || !source) {
        std::cerr << "[zith_debug_tokens] null arena or source\n";
        return;
    }

    std::vector<zith::detail::LexError> error_list;
    zith::detail::TokenList tokens;
    zith::detail::tokenize(std::string_view(source, source_len), arena, tokens, error_list);

    size_t count = 0;
    ZithToken *flat = tokens.flatten(arena, &count);

    // ── Header ───────────────────────────────────────────────────────────────
    std::cerr << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cerr << "║            Zith Tokenizer — Debug Dump              ║\n";
    std::cerr << "╠═══╦══════╦══════╦══════════════╦════════════════════════╣\n";
    std::cerr << "║ # ║ Line ║ Col  ║ Type         ║ Lexeme                 ║\n";
    std::cerr << "╠═══╩══════╩══════╩══════════════╩════════════════════════╣\n";

    // ── Rows ─────────────────────────────────────────────────────────────────
    for (size_t i = 0; i < count; ++i) {
        const ZithToken &tok = flat[i];

        // Truncate long lexemes for display
        char lexeme_buf[25];
        size_t lex_len = tok.lexeme.len < sizeof(lexeme_buf) - 1
                             ? tok.lexeme.len
                             : sizeof(lexeme_buf) - 4;
        std::memcpy(lexeme_buf, tok.lexeme.data, lex_len);
        if (lex_len < tok.lexeme.len) {
            lexeme_buf[lex_len++] = '.';
            lexeme_buf[lex_len++] = '.';
            lexeme_buf[lex_len++] = '.';
        }
        lexeme_buf[lex_len] = '\0';

        // Replace control characters for safe display
        for (size_t j = 0; j < lex_len; ++j)
            if (static_cast<unsigned char>(lexeme_buf[j]) < 0x20) lexeme_buf[j] = '?';

        const char *type_name = token_type_name(tok.type);

        std::cerr << "║ "
                << i
                << " \t║ "
                << tok.loc.line
                << " \t║ "
                << tok.loc.index
                << " \t║ "
                << type_name
                << " \t║ "
                << lexeme_buf
                << "\n";
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    std::cerr << "╚══════════════════════════════════════════════════════════╝\n";
    std::cerr << "  Total tokens : " << count << "\n";

    // ── Errors ───────────────────────────────────────────────────────────────
    if (!error_list.empty()) {
        std::cerr << "\n  Lexical errors (" << error_list.size() << "):\n";
        for (size_t i = 0; i < error_list.size(); ++i) {
            const auto &err = error_list[i];
            std::cerr << "  [" << i << "] line " << err.info.line
                    << ", col " << err.info.index
                    << " → " << err.msg << "\n";
        }
    } else {
        std::cerr << "  No lexical errors.\n";
    }
    std::cerr << '\n';
}

#undef ZITH_NEWLINE