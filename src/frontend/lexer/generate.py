#!/usr/bin/env python3
"""Generate lexer.hpp, lexer.cpp, actions.hpp, keyword-table.hpp and .gitignore
from lexer.rules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve()
while not (_REPO_ROOT / "tools").is_dir():
    _REPO_ROOT = _REPO_ROOT.parent
sys.path.insert(0, str(_REPO_ROOT))

from tools.rules_kit import (
    RuleError,
    cpp_char,
    cpp_string,
    join_logical_lines,
    parse_quoted,
    split_top_level,
)


BUCKET_COUNT = 128
TABLE_SIZE = 256
MAX_BUCKET_ITEMS = 16

TOKEN_KINDS = [
    "Identifier",
    "As",
    "Using",
    "Type",
    "Struct",
    "Raw",
    "Must",
    "Mutable",
    "Trait",
    "Interface",
    "Typedef",
    "Implement",
    "Fn",
    "Module",
    "Extern",
    "Macro",
    "Context",
    "Variable",
    "Ownership",
    "Yield",
    "Label",
    "Visibility",
    "If",
    "Else",
    "For",
    "In",
    "When",
    "Match",
    "Control",
    "Thread",
    "Error",
    "Drop",
    "Require",
    "Is",
    "Word",
    "Logical",
    "Comparison",
    "Operators",
    "Comments",
    "Docs",
    "Annotation",
    "Punctuation",
    "LitVal",
    "Unknown",
    "End",
]

PREFIX_DEFAULTS = {
    "hexadecimal": "0x",
    "binary": "0b",
    "octal": "0c",
}


def hash64(sv: str) -> int:
    h = 14695981039346656037
    for ch in sv.encode("utf-8"):
        h ^= ch
        h *= 1099511628211
        h &= (1 << 64) - 1
    return h


def mix64(x: int) -> int:
    mask = (1 << 64) - 1
    x ^= x >> 33
    x = (x * 0xFF51AFD7ED558CCD) & mask
    x ^= x >> 33
    x = (x * 0xC4CEB9FE1A85EC53) & mask
    x ^= x >> 33
    return x & mask


def dedupe_preserve_order(values: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def parse_strings(raw: str, line_no: int) -> list[str]:
    raw = raw.strip()
    if raw.startswith("[") and raw.endswith("]"):
        raw = raw[1:-1]
    values: list[str] = []
    for item in split_top_level(raw.strip(), line_no):
        if not item:
            continue
        values.append(parse_quoted(item, line_no))
    return values


def parse_options(raw: str, line_no: int) -> dict[str, str]:
    body = raw.strip()
    if not (body.startswith("[") and body.endswith("]")):
        body = f"[{body}]"
    inner = body[1:-1]
    if not inner.strip():
        return {}
    result: dict[str, str] = {}
    for item in split_top_level(inner, line_no):
        if "=" not in item:
            raise RuleError(line_no, f"opcao invalida: {item!r}")
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", key):
            raise RuleError(line_no, f"nome de opcao invalido: {key!r}")
        result[key] = value
    return result


def parse_bool_flag(raw: str, line_no: int, label: str) -> bool:
    lowered = raw.strip().lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    raise RuleError(line_no, f"{label} invalido: {raw!r}")


@dataclass
class TokenSpec:
    name: str
    line_no: int
    kind: str
    enabled: bool = True
    value: str = ""
    strings: list[str] = field(default_factory=list)
    options: dict[str, str] = field(default_factory=dict)

    def option_string(self, key: str, line_no: int) -> str:
        value = self.options.get(key)
        if value is None:
            return ""
        return parse_quoted(value, line_no)


@dataclass
class Member:
    name: str
    cpp_type: str
    initializer: str
    line_no: int


@dataclass
class Hook:
    qualified_name: str
    line_no: int

    @property
    def namespace(self) -> str:
        parts = self.qualified_name.rsplit("::", 1)
        return parts[0] if len(parts) == 2 else ""

    @property
    def name(self) -> str:
        return self.qualified_name.rsplit("::", 1)[-1]

    def declaration(self, params: str) -> str:
        return f"void {self.name}({params});"

    def call(self, args: str) -> str:
        return f"{self.qualified_name}({args});"


@dataclass
class LexerConfig:
    tokens: dict[str, TokenSpec]
    keywords: list[tuple[str, str]]
    lexer_members: list[Member]
    token_members: list[Member]
    on_lex: Hook | None
    off_lex: Hook | None
    on_token: Hook | None


def parse_hook(raw: str, line_no: int) -> Hook:
    match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_:]*)\s*\(\)", raw.strip())
    if not match:
        raise RuleError(line_no, f"forma de hook invalida: {raw!r}")
    return Hook(qualified_name=match.group(1), line_no=line_no)


def parse_member(raw: str, line_no: int) -> Member:
    if "=" not in raw:
        raise RuleError(line_no, f"member invalido (falta '='): {raw!r}")
    lhs, rhs = raw.split("=", 1)
    lhs = lhs.strip()
    rhs = rhs.strip()
    if lhs.endswith(":"):
        lhs = lhs[:-1].strip()
    if ":" not in lhs:
        raise RuleError(line_no, f"member sem tipo: {raw!r}")
    name, cpp_type = (part.strip() for part in lhs.split(":", 1))
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise RuleError(line_no, f"nome de member invalido: {name!r}")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:<>,. *&\[\]]*", cpp_type):
        raise RuleError(line_no, f"tipo de member invalido: {cpp_type!r}")
    return Member(name=name, cpp_type=cpp_type, initializer=rhs, line_no=line_no)


def parse_token_data(name: str, rhs: str, line_no: int) -> TokenSpec:
    lowered = rhs.lower()
    if lowered in {"true", "false"}:
        return TokenSpec(name, line_no, "bool", enabled=lowered == "true")

    if rhs.startswith("[") and rhs.endswith("]"):
        inner = rhs[1:-1]
        first_item = split_top_level(inner, line_no)[0] if inner.strip() else ""
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*\s*=.*", first_item):
            spec = TokenSpec(name, line_no, "options", options=parse_options(rhs, line_no))
            if name in PREFIX_DEFAULTS:
                spec.options.setdefault("prefix", PREFIX_DEFAULTS[name])
            return spec
        if name == "comments":
            return TokenSpec(name, line_no, "options", options=parse_options(rhs, line_no))
        return TokenSpec(name, line_no, "list", strings=parse_strings(inner, line_no))

    if (rhs.startswith('"') and rhs.endswith('"')) or (
        rhs.startswith("'") and rhs.endswith("'")
    ):
        return TokenSpec(name, line_no, "string", value=parse_quoted(rhs, line_no))

    raise RuleError(line_no, f"token invalido: {rhs!r}")


def parse_rules(text: str, path: Path) -> LexerConfig:
    section = ""
    tokens: dict[str, TokenSpec] = {}
    keywords: list[tuple[str, str]] = []
    lexer_members: list[Member] = []
    token_members: list[Member] = []
    hooks: dict[str, Hook] = {}
    token_type_line = 0

    logical = join_logical_lines(text)

    for line_no, line in logical:
        line = line.strip()
        if not line:
            continue

        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            section = header.group(1).strip().lower()
            if section == "token-type" and token_type_line == 0:
                token_type_line = line_no
            continue

        if section == "tokens":
            if "=" not in line:
                raise RuleError(line_no, f"token invalido: {line!r}")
            name, rhs = (part.strip() for part in line.split("=", 1))
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", name):
                raise RuleError(line_no, f"nome de token invalido: {name!r}")
            if name in tokens:
                raise RuleError(line_no, f"token repetido: {name!r}")
            tokens[name] = parse_token_data(name, rhs, line_no)
            continue

        if section == "keywords":
            if "=" not in line:
                raise RuleError(line_no, f"keyword invalida: {line!r}")
            kind, rhs = (part.strip() for part in line.split("=", 1))
            if kind not in TOKEN_KINDS:
                raise RuleError(line_no, f"TokenKind desconhecido: {kind!r}")
            if rhs.startswith("[") and rhs.endswith("]"):
                values = parse_strings(rhs[1:-1], line_no)
            else:
                values = [parse_quoted(rhs, line_no)]
            keywords.extend((kind, value) for value in values)
            continue

        if section == "lexer":
            lexer_members.append(parse_member(line, line_no))
            continue

        if section == "fields":
            lexer_members.append(parse_member(line, line_no))
            continue

        if section == "token-type":
            token_members.append(parse_member(line, line_no))
            continue

        if section == "actions":
            if "=" not in line:
                raise RuleError(line_no, f"hook invalido: {line!r}")
            name, rhs = (part.strip() for part in line.split("=", 1))
            if name not in {"onLex", "offLex", "onToken"}:
                raise RuleError(line_no, f"hook desconhecido: {name!r}")
            if name in hooks:
                raise RuleError(line_no, f"hook repetido: {name!r}")
            hooks[name] = parse_hook(rhs, line_no)
            continue

        raise RuleError(line_no, f"secao desconhecida: [{section}]")

    if token_type_line and "onToken" not in hooks:
        raise RuleError(token_type_line, "[token-type] requer [actions] onToken")

    identifier = tokens.get("identifier")
    if identifier is None or identifier.kind != "bool" or not identifier.enabled:
        raise RuleError(1, "token 'identifier' tem de estar habilitado")

    return LexerConfig(
        tokens=tokens,
        keywords=keywords,
        lexer_members=lexer_members,
        token_members=token_members,
        on_lex=hooks.get("onLex"),
        off_lex=hooks.get("offLex"),
        on_token=hooks.get("onToken"),
    )


def build_perfect_hash(keywords: list[tuple[str, str]]) -> tuple[list[int], list[int]]:
    words = [word for _, word in keywords]
    buckets: list[list[int]] = [[] for _ in range(BUCKET_COUNT)]
    for index, word in enumerate(words):
        buckets[hash64(word) % BUCKET_COUNT].append(index)

    for bucket in buckets:
        if len(bucket) > MAX_BUCKET_ITEMS:
            raise ValueError("perfect hash bucket overflow")

    seeds = [0] * BUCKET_COUNT
    table = [-1] * TABLE_SIZE
    for bucket_index, bucket in enumerate(buckets):
        if not bucket:
            continue
        placed = False
        for seed in range(255):
            slots: dict[int, int] = {}
            ok = True
            for keyword_index in bucket:
                slot = mix64(hash64(words[keyword_index]) ^ seed) % TABLE_SIZE
                if slot in slots or table[slot] != -1:
                    ok = False
                    break
                slots[slot] = keyword_index
            if ok:
                seeds[bucket_index] = seed
                for keyword_index in bucket:
                    slot = mix64(hash64(words[keyword_index]) ^ seed) % TABLE_SIZE
                    table[slot] = keyword_index
                placed = True
                break
        if not placed:
            raise ValueError("perfect hash placement failed; increase TableSize")
    return seeds, table


def format_int_array(values: list[int], per_line: int = 16) -> list[str]:
    lines: list[str] = []
    for start in range(0, len(values), per_line):
        lines.append(", ".join(str(value) for value in values[start : start + per_line]))
    return lines


def make_bool_table(enabled: set[int]) -> list[int]:
    return [1 if index in enabled else 0 for index in range(256)]


def make_keyword_header(keywords: list[tuple[str, str]]) -> str:
    seeds, table = build_perfect_hash(keywords)
    table_entries = ", ".join(
        f'{{"{word}", TokenKind::{kind}}}' for kind, word in keywords
    )
    lines = [
        "#pragma once",
        '#include "lexer.hpp"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace generated_lexer {",
        "namespace detail {",
        "",
        "constexpr uint64_t hash64(std::string_view sv) noexcept {",
        "    uint64_t h = 14695981039346656037ULL;",
        "    for (unsigned char c : sv)",
        "        h = (h ^ c) * 1099511628211ULL;",
        "    return h;",
        "}",
        "",
        "constexpr uint64_t mix64(uint64_t x) noexcept {",
        "    x ^= x >> 33;",
        "    x *= 0xFF51AFD7ED558CCDULL;",
        "    x ^= x >> 33;",
        "    x *= 0xC4CEB9FE1A85EC53ULL;",
        "    x ^= x >> 33;",
        "    return x;",
        "}",
        "",
        f"constexpr std::array<std::pair<std::string_view, TokenKind>, {len(keywords)}>",
        f"    keyword_table = {{{{{table_entries}}}}};",
        "",
        f"constexpr std::array<int16_t, {TABLE_SIZE}> hash_table = {{",
    ]
    lines.extend(f"    {line}," for line in format_int_array(table))
    lines.append(f"}};")
    lines.append("")
    lines.append(f"constexpr std::array<uint8_t, {BUCKET_COUNT}> bucket_seed = {{")
    lines.extend(f"    {line}," for line in format_int_array(seeds))
    lines.append(f"}};")
    lines.append("")
    lines.extend(
        [
            "constexpr bool hasDuplicateKeywords() noexcept {",
            "    for (size_t i = 0; i < keyword_table.size(); ++i)",
            "        for (size_t j = i + 1; j < keyword_table.size(); ++j)",
            "            if (keyword_table[i].first == keyword_table[j].first)",
            "                return true;",
            "    return false;",
            "}",
            "static_assert(!hasDuplicateKeywords(), \"keyword_table contains duplicate keywords\");",
            "",
            f"constexpr bool hasBucketOverflow() noexcept {{",
            f"    std::array<uint8_t, {BUCKET_COUNT}> counts{{}};",
            "    for (const auto &entry : keyword_table) {",
            f"        const size_t b = hash64(entry.first) % {BUCKET_COUNT};",
            f"        if (++counts[b] > {MAX_BUCKET_ITEMS})",
            "            return true;",
            "    }",
            "    return false;",
            "}",
            'static_assert(!hasBucketOverflow(), "bucket overflow (>16 keywords per bucket)");',
            "",
            "constexpr bool allKeywordsPlaced() noexcept {",
            "    for (const auto &entry : keyword_table) {",
            f"        const size_t b = hash64(entry.first) % {BUCKET_COUNT};",
            f"        const size_t slot = mix64(hash64(entry.first) ^ bucket_seed[b]) % {TABLE_SIZE};",
            "        const int16_t id = hash_table[slot];",
            "        if (id < 0 || keyword_table[static_cast<size_t>(id)].first != entry.first)",
            "            return false;",
            "    }",
            "    return true;",
            "}",
            "static_assert(allKeywordsPlaced(), \"not all keywords placed in perfect hash\");",
            "",
            "} // namespace detail",
            "",
            "inline TokenKind lookupKeyword(std::string_view sv) noexcept {",
            "    if (sv.empty()) return TokenKind::Identifier;",
            f"    const size_t b = detail::hash64(sv) % {BUCKET_COUNT};",
            f"    const size_t slot =",
            f"        detail::mix64(detail::hash64(sv) ^ detail::bucket_seed[b]) % {TABLE_SIZE};",
            "    const int16_t id = detail::hash_table[slot];",
            "    if (id < 0) return TokenKind::Identifier;",
            "    return (detail::keyword_table[static_cast<size_t>(id)].first == sv)",
            "               ? detail::keyword_table[static_cast<size_t>(id)].second",
            "               : TokenKind::Identifier;",
            "}",
            "",
            "} // namespace generated_lexer",
            "",
        ]
    )
    return "\n".join(lines)


def make_lexer_header(config: LexerConfig) -> str:
    lines = [
        "#pragma once",
        '#include "types.hpp"',
        '#include "common/memory/string-interner.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstdio>",
        "#include <span>",
        "#include <string_view>",
        "#include <vector>",
        "",
        "namespace generated_lexer {",
        "",
        "using InternedId = common::memory::InternedId;",
        "using StringInterner = common::memory::StringInterner;",
        "",
        "enum class TokenKind : uint8_t {",
    ]
    lines.extend(f"    {kind}," for kind in TOKEN_KINDS)
    lines.extend(
        [
            "};",
            "",
            "struct Token {",
            "    Span span = Span(0, 0);",
            "    TokenKind kind = TokenKind::Unknown;",
            "    char punc = 0;",
            "    InternedId lexemeId = 0;",
        ]
    )
    for member in config.token_members:
        lines.append(f"    {member.cpp_type} {member.name} = {member.initializer};")
    lines.extend(
        [
            "",
            "    Token() = default;",
            "    Token(Span span_, TokenKind kind_, char punc_ = 0)",
            "        : span(span_), kind(kind_), punc(punc_) {}",
            "",
            "    [[nodiscard]] constexpr bool is(TokenKind k) const noexcept {",
            "        return kind == k;",
            "    }",
            "    [[nodiscard]] constexpr bool is_eof() const noexcept {",
            "        return kind == TokenKind::End;",
            "    }",
            "};",
            "",
            "inline const Token &terminalToken() noexcept {",
            "    static const Token end{};",
            "    return end;",
            "}",
            "",
            "struct TokenStream {",
            "    std::vector<Token> tokens;",
            "    const common::memory::StringInterner *strings = nullptr;",
            "    const Token *src = nullptr;",
            "    uint32_t len = 0;",
            "    uint32_t offset = 0;",
            "",
            "    TokenStream() = default;",
            "    TokenStream(std::vector<Token> tokens_, const StringInterner &strings_)",
            "        : tokens(std::move(tokens_)), strings(&strings_),",
            "          src(tokens.data()), len(static_cast<uint32_t>(tokens.size())) {}",
            "    explicit TokenStream(",
            "        std::span<const Token> tokens_, const StringInterner &strings_)",
            "        : tokens(tokens_.begin(), tokens_.end()), strings(&strings_),",
            "          src(tokens.data()), len(static_cast<uint32_t>(tokens.size())) {}",
            "",
            "    [[nodiscard]] size_t size() const noexcept { return len; }",
            "    [[nodiscard]] bool empty() const noexcept { return len == 0; }",
            "    void reset() noexcept { offset = 0; }",
            "    [[nodiscard]] bool hasNext() const noexcept { return offset < len; }",
            "    [[nodiscard]] const Token &current() const noexcept { return peek(); }",
            "    [[nodiscard]] const Token &at(size_t index) const noexcept {",
            "        return index < len ? src[index] : terminalToken();",
            "    }",
            "    [[nodiscard]] const Token &operator[](size_t index) const noexcept {",
            "        return at(index);",
            "    }",
            "    [[nodiscard]] std::span<const Token> absoluteSlice(",
            "        size_t start, size_t end",
            "    ) const noexcept {",
            "        if (start >= end || start >= len || src == nullptr) return {};",
            "        end = end > len ? len : end;",
            "        return {src + start, end - start};",
            "    }",
            "    [[nodiscard]] std::span<const Token> slice(",
            "        size_t start, size_t count",
            "    ) const noexcept {",
            "        if (count == 0 || offset + start >= len || src == nullptr) return {};",
            "        return absoluteSlice(offset + start, offset + start + count);",
            "    }",
            "",
            "    [[nodiscard]] std::string_view lexeme(const Token &t) const noexcept {",
            "        return lexeme(t.lexemeId);",
            "    }",
            "    [[nodiscard]] std::string_view lexeme(InternedId id) const noexcept {",
            "        if (strings == nullptr)",
            "            return {};",
            "        return strings->lookup(id);",
            "    }",
            "    [[nodiscard]] const Token &peek() const noexcept;",
            "    void advance() noexcept;",
            "    bool match(TokenKind kind) noexcept {",
            "        if (!hasNext() || !current().is(kind)) return false;",
            "        advance();",
            "        return true;",
            "    }",
            "    template <typename First, typename... Rest>",
            "    bool match(First first, Rest... rest) noexcept {",
            "        if (!match(first)) return false;",
            "        if (!match(rest...)) return false;",
            "        return true;",
            "    }",
            "    bool matchLexeme(std::string_view lexeme) noexcept {",
            "        if (!hasNext() || this->lexeme(current()) != lexeme) return false;",
            "        advance();",
            "        return true;",
            "    }",
            "    template <typename... Args>",
            "    bool matchAll(Args... args) noexcept {",
            "        const size_t saved = offset;",
            "        if ((match(args) && ...)) return true;",
            "        offset = saved;",
            "        return false;",
            "    }",
            "    template <typename... Args>",
            "    bool matchAllLexeme(Args... args) noexcept {",
            "        const size_t saved = offset;",
            "        if ((matchLexeme(args) && ...)) return true;",
            "        offset = saved;",
            "        return false;",
            "    }",
            "",
            "};",
            "",
            "class Lexer {",
            "public:",
            "    Lexer();",
            "    [[nodiscard]] TokenStream run(",
            "        std::string_view source, common::memory::StringInterner &strings",
            "    );",
            "    [[nodiscard]] std::string_view span_slice(",
            "        std::string_view source, const Span &span) const noexcept;",
            "    [[nodiscard]] TokenKind lookupKeyword(std::string_view word) const noexcept;",
            "",
        ]
    )
    for member in config.lexer_members:
        lines.append(f"    {member.cpp_type} {member.name} = {member.initializer};")
    lines.extend(
        [
            "",
            "private:",
            "    std::string_view source_;",
            "    size_t offset_ = 0;",
            "};",
            "",
            "[[nodiscard]] TokenStream tokenize(",
            "    std::string_view source, common::memory::StringInterner &strings",
            ");",
            "const char *tokenKindName(TokenKind kind) noexcept;",
            "struct FormattedToken {",
            "    InternedId kindId = 0;",
            "    InternedId lexemeId = 0;",
            "    Span span = Span(0, 0);",
            "};",
            "FormattedToken formatToken(",
            "    const Token &token, common::memory::StringInterner &strings",
            ");",
            "FormattedToken formatToken(",
            "    const TokenStream &stream, size_t index",
            ");",
            "void printToken(",
            "    FILE *out, const FormattedToken &formatted, const StringInterner &strings",
            ");",
            "void printToken(FILE *out, const TokenStream &stream);",
            "void printToken(",
            "    FILE *out, std::string_view source, common::memory::StringInterner &strings",
            ");",
            "",
            "} // namespace generated_lexer",
            "",
        ]
    )
    return "\n".join(lines)


def make_lexer_source(config: LexerConfig) -> str:
    tokens = config.tokens
    skip_spec = tokens.get("skip")
    skip_chars = skip_spec.value if skip_spec and skip_spec.kind == "string" else ""

    punc_spec = tokens.get("punc")
    punc_chars = punc_spec.value if punc_spec and punc_spec.kind == "string" else ""
    op_spec = tokens.get("operators")
    op_chars = op_spec.value if op_spec and op_spec.kind == "string" else ""
    compound_spec = tokens.get("compound")
    compound = list(compound_spec.strings) if compound_spec else []
    compound = dedupe_preserve_order(compound)
    compound.sort(key=lambda item: (-len(item), item))

    string_spec = tokens.get("string")
    string_opts = string_spec.options if string_spec else {}
    escape_sequences = string_opts.get("escape-sequence", "true").lower() == "true"
    decimal_spec = tokens.get("decimal")
    decimal_underscore = False
    if decimal_spec and decimal_spec.kind == "options":
        raw = decimal_spec.options.get("under-score-divisor")
        if raw is not None:
            decimal_underscore = parse_bool_flag(
                raw, decimal_spec.line_no, "under-score-divisor"
            )

    comments_spec = tokens.get("comments")
    single_comment = ""
    multi_open = ""
    multi_close = ""
    if comments_spec:
        single_comment = comments_spec.option_string("single", comments_spec.line_no)
        multi_raw = comments_spec.options.get("multi", "")
        if multi_raw:
            pair = parse_strings(multi_raw, comments_spec.line_no)
            if len(pair) != 2:
                raise RuleError(
                    comments_spec.line_no, "comments multi precisa de [open, close]"
                )
            multi_open, multi_close = pair

    number_prefixes: list[tuple[str, str]] = []
    for name, table_name in (
        ("hexadecimal", "hex_digit_table"),
        ("octal", "octal_digit_table"),
        ("binary", "binary_digit_table"),
    ):
        spec = tokens.get(name)
        if spec is None:
            continue
        enabled = False
        prefix = PREFIX_DEFAULTS[name]
        if spec.kind == "bool":
            enabled = spec.enabled
        else:
            enabled = True
            option_prefix = spec.option_string("prefix", spec.line_no)
            if option_prefix:
                prefix = option_prefix
        if enabled:
            number_prefixes.append((prefix, table_name))

    skip_bytes = {ord(ch) for ch in skip_chars}
    op_bytes = {ord(ch) for ch in op_chars}
    punc_bytes = {ord(ch) for ch in punc_chars}

    complex_bytes = {ord('"'), ord("'"), ord("_")}
    complex_bytes.update(range(ord("0"), ord("9") + 1))
    complex_bytes.update(range(ord("a"), ord("z") + 1))
    complex_bytes.update(range(ord("A"), ord("Z") + 1))
    complex_bytes.update(ord(op[0]) for op in compound if op)
    if single_comment:
        complex_bytes.add(ord(single_comment[0]))
    if multi_open:
        complex_bytes.add(ord(multi_open[0]))

    simple_tag_table = [0] * 256
    for index in range(128):
        if index in complex_bytes:
            simple_tag_table[index] = 0
        elif index in skip_bytes:
            simple_tag_table[index] = 1
        elif index in op_bytes:
            simple_tag_table[index] = 2
        elif index in punc_bytes:
            simple_tag_table[index] = 3
        else:
            simple_tag_table[index] = 4

    identifier_tail = set(range(ord("a"), ord("z") + 1))
    identifier_tail.update(range(ord("A"), ord("Z") + 1))
    identifier_tail.update(range(ord("0"), ord("9") + 1))
    identifier_tail.add(ord("_"))
    decimal_digits = set(range(ord("0"), ord("9") + 1))
    hex_digits = set(decimal_digits)
    hex_digits.update(range(ord("a"), ord("f") + 1))
    hex_digits.update(range(ord("A"), ord("F") + 1))
    octal_digits = set(range(ord("0"), ord("7") + 1))
    binary_digits = {ord("0"), ord("1")}

    lines = [
        '#include "lexer.hpp"',
        '#include "keyword-table.hpp"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstdio>",
        "#include <string>",
        "#include <utility>",
        "",
        "namespace generated_lexer {",
        "",
        "namespace detail {",
        "",
        "enum class SimpleTag : uint8_t {",
        "    Complex = 0,",
        "    Skip = 1,",
        "    Operator = 2,",
        "    Punctuation = 3,",
        "    Unknown = 4,",
        "};",
        "",
        "struct PendingToken {",
        "    uint32_t start = 0;",
        "    uint32_t end = 0;",
        "    TokenKind kind = TokenKind::Unknown;",
        "    char punc = 0;",
        "};",
        "",
        "constexpr size_t kFastChunkBytes = 64;",
        "constexpr size_t kRunChunkBytes = 16;",
        "",
        "constexpr std::array<uint8_t, 256> space_table = {",
    ]
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(skip_bytes))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> identifier_tail_table = {",
        ]
    )
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(identifier_tail))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> decimal_digit_table = {",
        ]
    )
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(decimal_digits))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> hex_digit_table = {",
        ]
    )
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(hex_digits))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> octal_digit_table = {",
        ]
    )
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(octal_digits))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> binary_digit_table = {",
        ]
    )
    lines.extend(
        f"    {line}," for line in format_int_array(make_bool_table(binary_digits))
    )
    lines.extend(
        [
            "};",
            "",
            "constexpr std::array<uint8_t, 256> simple_tag_table = {",
        ]
    )
    lines.extend(f"    {line}," for line in format_int_array(simple_tag_table))
    lines.extend(
        [
            "};",
            "",
            "inline bool table_contains(",
            "    const std::array<uint8_t, 256> &table, unsigned char ch",
            ") noexcept {",
            "    return table[ch] != 0;",
            "}",
            "",
            "inline bool block_matches(",
            "    const char *text, size_t offset, const std::array<uint8_t, 256> &table",
            ") noexcept {",
            "    return table_contains(table, static_cast<unsigned char>(text[offset + 0])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 1])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 2])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 3])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 4])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 5])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 6])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 7])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 8])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 9])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 10])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 11])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 12])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 13])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 14])) &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset + 15]));",
            "}",
            "",
            "inline size_t consume_table_run(",
            "    const char *text, size_t size, size_t offset,",
            "    const std::array<uint8_t, 256> &table",
            ") noexcept {",
            "    while (offset + kRunChunkBytes <= size && block_matches(text, offset, table))",
            "        offset += kRunChunkBytes;",
            "    while (offset < size && table_contains(table, static_cast<unsigned char>(text[offset])))",
            "        ++offset;",
            "    return offset;",
            "}",
            "",
            "inline size_t consume_ascii_space(",
            "    const char *text, size_t size, size_t offset",
            ") noexcept {",
            "    return consume_table_run(text, size, offset, space_table);",
            "}",
            "",
            "inline size_t consume_identifier_tail(",
            "    const char *text, size_t size, size_t offset",
            ") noexcept {",
            "    return consume_table_run(text, size, offset, identifier_tail_table);",
            "}",
            "",
            "template <typename Emit>",
            "inline void flush_pending(",
            "    const PendingToken *pending, size_t count, Emit &&emit",
            ") {",
            "    for (size_t index = 0; index < count; ++index) {",
            "        const PendingToken &item = pending[index];",
            "        emit(item.kind, Span{item.start, item.end}, item.punc);",
            "    }",
            "}",
            "",
            "inline size_t scan_simple_chunk(",
            "    const char *text, size_t size, size_t offset, PendingToken *pending, size_t &count",
            ") noexcept {",
            "    const size_t start = offset;",
            "    const size_t limit = offset + kFastChunkBytes < size ? offset + kFastChunkBytes : size;",
            "    while (offset < limit) {",
            "        const unsigned char ch = static_cast<unsigned char>(text[offset]);",
            "        if (ch >= 128)",
            "            break;",
            "        const SimpleTag tag = static_cast<SimpleTag>(simple_tag_table[ch]);",
            "        switch (tag) {",
            "        case SimpleTag::Skip:",
            "            ++offset;",
            "            continue;",
            "        case SimpleTag::Operator:",
            "            pending[count++] = PendingToken{",
            "                static_cast<uint32_t>(offset),",
            "                static_cast<uint32_t>(offset + 1),",
            "                TokenKind::Operators,",
            "                static_cast<char>(ch),",
            "            };",
            "            ++offset;",
            "            continue;",
            "        case SimpleTag::Punctuation:",
            "            pending[count++] = PendingToken{",
            "                static_cast<uint32_t>(offset),",
            "                static_cast<uint32_t>(offset + 1),",
            "                TokenKind::Punctuation,",
            "                static_cast<char>(ch),",
            "            };",
            "            ++offset;",
            "            continue;",
            "        case SimpleTag::Unknown:",
            "            pending[count++] = PendingToken{",
            "                static_cast<uint32_t>(offset),",
            "                static_cast<uint32_t>(offset + 1),",
            "                TokenKind::Unknown,",
            "                0,",
            "            };",
            "            ++offset;",
            "            continue;",
            "        case SimpleTag::Complex:",
            "            break;",
            "        }",
            "        break;",
            "    }",
            "    return offset == start ? start : offset;",
            "}",
            "",
            "inline bool is_ascii_alpha(unsigned char c) noexcept {",
            "    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');",
            "}",
            "",
            "inline bool is_identifier_start(unsigned char c) noexcept {",
            "    return is_ascii_alpha(c) || c == '_';",
            "}",
            "",
            "inline bool prefix_matches(std::string_view rest, std::string_view prefix) noexcept {",
            "    return rest.size() >= prefix.size() && rest.substr(0, prefix.size()) == prefix;",
            "}",
            "",
            "template <typename Table>",
            "inline bool consume_digit_run(",
            "    const char *text, size_t size, size_t &offset, bool allow_underscore,",
            "    const Table &digits",
            ") noexcept {",
            "    bool saw_digit = false;",
            "    bool prev_underscore = false;",
            "    bool valid = true;",
            "    while (offset < size) {",
            "        if (!prev_underscore && offset + kRunChunkBytes <= size &&",
            "            block_matches(text, offset, digits)) {",
            "            saw_digit = true;",
            "            offset += kRunChunkBytes;",
            "            continue;",
            "        }",
            "        const unsigned char ch = static_cast<unsigned char>(text[offset]);",
            "        if (table_contains(digits, ch)) {",
            "            saw_digit = true;",
            "            prev_underscore = false;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        if (allow_underscore && ch == '_') {",
            "            if (!saw_digit || prev_underscore)",
            "                valid = false;",
            "            prev_underscore = true;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        break;",
            "    }",
            "    if (!saw_digit || prev_underscore)",
            "        valid = false;",
            "    return valid;",
            "}",
            "",
            "} // namespace detail",
            "",
        ]
    )
    if config.on_lex or config.off_lex or config.on_token:
        lines.insert(2, '#include "actions.hpp"')
        lines.insert(3, "")

    if compound:
        lines.extend(
            [
                f"constexpr std::array<std::string_view, {len(compound)}> compound_set = {{",
                f"    {', '.join(cpp_string(item) for item in compound)},",
                "};",
                "",
            ]
        )

    lines.extend(
        [
            "const Token &TokenStream::peek() const noexcept {",
            "    if (offset >= len) return terminalToken();",
            "    return src[offset];",
            "}",
            "",
            "void TokenStream::advance() noexcept {",
            "    if (offset < len) ++offset;",
            "}",
            "",
            "Lexer::Lexer()",
        ]
    )
    init = [f"{member.name}({member.initializer})" for member in config.lexer_members]
    if init:
        lines.append("    : " + ",\n      ".join(init))
    lines.extend(
        [
            "{",
            "}",
            "",
            "std::string_view Lexer::span_slice(",
            "    std::string_view source, const Span &span) const noexcept {",
            "    if (span.end <= source.size())",
            "        return source.substr(span.start, span.end - span.start);",
            "    return {};",
            "}",
            "",
            "TokenKind Lexer::lookupKeyword(std::string_view word) const noexcept {",
            "    return generated_lexer::lookupKeyword(word);",
            "}",
            "",
            "TokenStream Lexer::run(",
            "    std::string_view source, common::memory::StringInterner &strings",
            ") {",
            "    source_ = source;",
            "    offset_ = 0;",
            "    std::vector<Token> tokens;",
            "    tokens.reserve(source.size() / 4 + 8);",
            "",
            "    const char *text = source.data();",
            "    const size_t size = source.size();",
            "",
            "    const auto span = [](size_t start, size_t end) {",
            "        return Span{static_cast<uint32_t>(start), static_cast<uint32_t>(end)};",
            "    };",
        ]
    )
    if config.on_token:
        lines.extend(
            [
                "    const auto emit = [&](TokenKind kind, Span s, char punc = 0) {",
                "        Token token(s, kind, punc);",
                "        const std::string_view lexeme = span_slice(source, s);",
                "        token.lexemeId = strings.intern(lexeme);",
                f"        {config.on_token.call('*this, std::as_const(token), lexeme')}",
                "        tokens.push_back(token);",
                "    };",
            ]
        )
    else:
        lines.extend(
            [
                "    const auto emit = [&](TokenKind kind, Span s, char punc = 0) {",
                "        Token token(s, kind, punc);",
                "        token.lexemeId = strings.intern(span_slice(source, s));",
                "        tokens.push_back(token);",
                "    };",
            ]
        )
    if config.on_lex:
        lines.extend(["", f"    {config.on_lex.call('*this, source')}"])
    lines.extend(
        [
            "",
            "    while (offset_ < size) {",
            "        offset_ = detail::consume_ascii_space(text, size, offset_);",
            "        if (offset_ >= size)",
            "            break;",
            "",
            "        detail::PendingToken pending[detail::kFastChunkBytes];",
            "        size_t pending_count = 0;",
            "        const size_t fast_end = detail::scan_simple_chunk(",
            "            text, size, offset_, pending, pending_count",
            "        );",
            "        if (fast_end != offset_) {",
            "            detail::flush_pending(pending, pending_count, emit);",
            "            offset_ = fast_end;",
            "            continue;",
            "        }",
            "",
            "        const size_t before = offset_;",
            "        const char c = text[offset_];",
            "        const std::string_view rest(text + offset_, size - offset_);",
            "",
        ]
    )

    if single_comment:
        lines.extend(
            [
                f"        if (detail::prefix_matches(rest, {cpp_string(single_comment)})) {{",
                f"            offset_ += {len(single_comment)};",
                "            while (offset_ < size && text[offset_] != '\\n')",
                "                ++offset_;",
                "            emit(TokenKind::Comments, span(before, offset_));",
                "            continue;",
                "        }",
                "",
            ]
        )
    if multi_open and multi_close:
        lines.extend(
            [
                f"        if (detail::prefix_matches(rest, {cpp_string(multi_open)})) {{",
                f"            offset_ += {len(multi_open)};",
                f"            const size_t close_at = rest.find({cpp_string(multi_close)}, {len(multi_open)});",
                "            if (close_at == std::string_view::npos)",
                "                offset_ = size;",
                "            else",
                f"                offset_ = before + close_at + {len(multi_close)};",
                "            emit(TokenKind::Comments, span(before, offset_));",
                "            continue;",
                "        }",
                "",
            ]
        )

    lines.extend(
        [
            "        if (c == '\"' || c == '\\'') {",
            "            bool valid = true;",
            "            bool terminated = false;",
            "            ++offset_;",
        ]
    )
    if escape_sequences:
        lines.extend(
            [
                "            while (offset_ < size) {",
                "                if (text[offset_] == '\\n' || text[offset_] == '\\r') {",
                "                    valid = false;",
                "                    break;",
                "                }",
                "                if (text[offset_] == '\\\\') {",
                "                    if (offset_ + 1 >= size) {",
                "                        ++offset_;",
                "                        valid = false;",
                "                        break;",
                "                    }",
                "                    switch (text[offset_ + 1]) {",
                "                    case '\\\\':",
                "                    case '\"':",
                "                    case '\\'':",
                "                    case 'n':",
                "                    case 'r':",
                "                    case 't':",
                "                    case '0':",
                "                        offset_ += 2;",
                "                        continue;",
                "                    default:",
                "                        valid = false;",
                "                        offset_ += 2;",
                "                        continue;",
                "                    }",
                "                }",
                "                if (text[offset_] == c) {",
                "                    ++offset_;",
                "                    terminated = true;",
                "                    break;",
                "                }",
                "                ++offset_;",
                "            }",
            ]
        )
    else:
        lines.extend(
            [
                "            while (offset_ < size) {",
                "                if (text[offset_] == '\\n' || text[offset_] == '\\r') {",
                "                    valid = false;",
                "                    break;",
                "                }",
                "                if (text[offset_] == c) {",
                "                    ++offset_;",
                "                    terminated = true;",
                "                    break;",
                "                }",
                "                ++offset_;",
                "            }",
            ]
        )
    lines.extend(
        [
            "            if (!terminated)",
            "                valid = false;",
            "            emit(valid ? TokenKind::LitVal : TokenKind::Unknown,",
            "                 span(before, offset_));",
            "            continue;",
            "        }",
            "",
            "        if (c >= '0' && c <= '9') {",
        ]
    )
    for prefix, table_name in number_prefixes:
        lines.extend(
            [
                f"            if (detail::prefix_matches(rest, {cpp_string(prefix)})) {{",
                f"                offset_ += {len(prefix)};",
                f"                const bool valid = detail::consume_digit_run(",
                f"                    text, size, offset_, true, detail::{table_name}",
                "                );",
                "                emit(valid ? TokenKind::LitVal : TokenKind::Unknown,",
                "                     span(before, offset_));",
                "                continue;",
                "            }",
            ]
        )
    lines.extend(
        [
            "            const bool integer_valid = detail::consume_digit_run(",
            f"                text, size, offset_, {'true' if decimal_underscore else 'false'}, detail::decimal_digit_table",
            "            );",
            "            if (offset_ < size && text[offset_] == '.' &&",
            "                offset_ + 1 < size &&",
            "                detail::table_contains(",
            "                    detail::decimal_digit_table,",
            "                    static_cast<unsigned char>(text[offset_ + 1])",
            "                )) {",
            "                ++offset_;",
            "                const bool fraction_valid = detail::consume_digit_run(",
            f"                    text, size, offset_, {'true' if decimal_underscore else 'false'}, detail::decimal_digit_table",
            "                );",
            "                emit(integer_valid && fraction_valid ? TokenKind::LitVal",
            "                                                  : TokenKind::Unknown,",
            "                     span(before, offset_));",
            "                continue;",
            "            }",
            "            emit(integer_valid ? TokenKind::LitVal : TokenKind::Unknown,",
            "                 span(before, offset_));",
            "            continue;",
            "        }",
            "",
            "        if (detail::is_identifier_start(static_cast<unsigned char>(c))) {",
            "            ++offset_;",
            "            offset_ = detail::consume_identifier_tail(text, size, offset_);",
            "            const std::string_view word(text + before, offset_ - before);",
            "            emit(lookupKeyword(word), span(before, offset_));",
            "            continue;",
            "        }",
            "",
        ]
    )
    if compound:
        lines.extend(
            [
                "        bool matched_compound = false;",
                "        for (const std::string_view op : compound_set) {",
                "            if (detail::prefix_matches(rest, op)) {",
                "                offset_ += op.size();",
                "                emit(TokenKind::Operators, span(before, offset_));",
                "                matched_compound = true;",
                "                break;",
                "            }",
                "        }",
                "        if (matched_compound)",
                "            continue;",
                "",
            ]
        )

    if op_chars:
        lines.extend(["        switch (c) {"])
        for ch in dedupe_preserve_order(list(op_chars)):
            lines.append(f"        case {cpp_char(ch)}:")
        lines.extend(
            [
                "            ++offset_;",
                "            emit(TokenKind::Operators, span(before, offset_), c);",
                "            continue;",
                "        default:",
                "            break;",
                "        }",
                "",
            ]
        )

    if punc_chars:
        lines.extend(["        switch (c) {"])
        for ch in dedupe_preserve_order(list(punc_chars)):
            lines.append(f"        case {cpp_char(ch)}:")
        lines.extend(
            [
                "            ++offset_;",
                "            emit(TokenKind::Punctuation, span(before, offset_), c);",
                "            continue;",
                "        default:",
                "            break;",
                "        }",
                "",
            ]
        )

    lines.extend(
        [
            "        ++offset_;",
            "        emit(TokenKind::Unknown, span(before, offset_));",
            "    }",
            "",
            "    emit(TokenKind::End, span(size, size));",
        ]
    )
    if config.off_lex:
        lines.append(f"    {config.off_lex.call('*this, source, tokens')}")
    lines.extend(
        [
            "    return TokenStream(std::move(tokens), strings);",
            "}",
            "",
            "TokenStream tokenize(",
            "    std::string_view source, common::memory::StringInterner &strings",
            ") {",
            "    Lexer lexer;",
            "    return lexer.run(source, strings);",
            "}",
            "",
            "const char *tokenKindName(TokenKind kind) noexcept {",
            "    static constexpr const char *names[] = {",
        ]
    )
    lines.extend(f'        "{kind}",' for kind in TOKEN_KINDS)
    lines.extend(
        [
            "    };",
            "    static_assert(std::size(names) == static_cast<size_t>(TokenKind::End) + 1,",
            '                  "tokenKindName array must match TokenKind enum");',
            "    const size_t index = static_cast<size_t>(kind);",
            "    return index < std::size(names) ? names[index] : \"???\";",
            "}",
            "",
            "FormattedToken formatToken(",
            "    const Token &token, common::memory::StringInterner &strings",
            ") {",
            "    return FormattedToken{",
            "        strings.intern(tokenKindName(token.kind)), token.lexemeId, token.span,",
            "    };",
            "}",
            "",
            "FormattedToken formatToken(",
            "    const TokenStream &stream, size_t index",
            ") {",
            "    const Token &token = stream.at(index);",
            "    if (stream.strings != nullptr) {",
            "        common::memory::StringInterner *strings =",
            "            const_cast<common::memory::StringInterner *>(stream.strings);",
            "        return formatToken(token, *strings);",
            "    }",
            "    return FormattedToken{0, 0, {0, 0}};",
            "}",
            "",
            "void printToken(",
            "    FILE *out, const FormattedToken &formatted, const StringInterner &strings",
            ") {",
            "    const std::string_view kind = strings.lookup(formatted.kindId);",
            "    const std::string_view lexeme = strings.lookup(formatted.lexemeId);",
            "    std::fwrite(kind.data(), 1, kind.size(), out);",
            "    std::fputc('(', out);",
            "    std::fwrite(lexeme.data(), 1, lexeme.size(), out);",
            "    std::fprintf(out, \"): [%u,%u]\\n\",",
            "                 formatted.span.start, formatted.span.end);",
            "}",
            "",
            "void printToken(FILE *out, const TokenStream &stream) {",
            "    if (stream.strings != nullptr) {",
            "        common::memory::StringInterner *strings =",
            "            const_cast<common::memory::StringInterner *>(stream.strings);",
            "        for (size_t i = 0; i < stream.size(); ++i)",
            "            printToken(out, formatToken(stream, i), *strings);",
            "        return;",
            "    }",
            "    for (const Token &token : stream.absoluteSlice(0, stream.size())) {",
            "        const std::string_view lexeme = stream.lexeme(token);",
            "        std::fputs(tokenKindName(token.kind), out);",
            "        std::fputc('(', out);",
            "        std::fwrite(lexeme.data(), 1, lexeme.size(), out);",
            "        std::fprintf(out, \"): [%u,%u)\\n\",",
            "                     token.span.start, token.span.end);",
            "    }",
            "}",
            "",
            "void printToken(",
            "    FILE *out, std::string_view source, common::memory::StringInterner &strings",
            ") {",
            "    TokenStream stream = tokenize(source, strings);",
            "    printToken(out, stream);",
            "}",
            "",
            "} // namespace generated_lexer",
            "",
        ]
    )
    return "\n".join(lines)


def make_actions_header(config: LexerConfig) -> str:
    hooks = [hook for hook in (config.on_lex, config.off_lex, config.on_token) if hook]
    if not hooks:
        return "#pragma once\n"
    declarations: list[tuple[Hook, str]] = []
    if config.on_lex:
        declarations.append(
            (
                config.on_lex,
                config.on_lex.declaration(
                    "generated_lexer::Lexer &lexer, std::string_view source"
                ),
            )
        )
    if config.on_token:
        declarations.append(
            (
                config.on_token,
                config.on_token.declaration(
                    "generated_lexer::Lexer &lexer, const generated_lexer::Token &token, "
                    "std::string_view lexeme"
                ),
            )
        )
    if config.off_lex:
        declarations.append(
            (
                config.off_lex,
                config.off_lex.declaration(
                    "generated_lexer::Lexer &lexer, std::string_view source, "
                    "const generated_lexer::TokenStream &tokens"
                ),
            )
        )

    lines = ['#pragma once', '', '#include "lexer.hpp"', ""]
    for index, (hook, declaration) in enumerate(declarations):
        if hook.namespace:
            lines.append(f"namespace {hook.namespace} {{ {declaration} }}")
        else:
            lines.append(declaration)
        if index + 1 != len(declarations):
            lines.append("")
    return "\n".join(lines) + "\n"


def make_gitignore() -> str:
    return "lexer.hpp\nlexer.cpp\nactions.hpp\nkeyword-table.hpp\n__pycache__/\n"


def generated_files(config: LexerConfig) -> list[tuple[str, str]]:
    return [
        ("lexer.hpp", make_lexer_header(config)),
        ("lexer.cpp", make_lexer_source(config)),
        ("actions.hpp", make_actions_header(config)),
        ("keyword-table.hpp", make_keyword_header(config.keywords)),
        (".gitignore", make_gitignore()),
    ]


def validate_types(config: LexerConfig, types_path: Path) -> None:
    text = types_path.read_text(encoding="utf-8")
    declared = set()
    builtin_types = {
        "void",
        "bool",
        "char",
        "short",
        "int",
        "long",
        "float",
        "double",
        "size_t",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
    }
    for keyword in ("struct", "class", "enum", "typedef", "using"):
        declared.update(
            name
            for name in re.findall(
                rf"\b{keyword}\s+([A-Za-z_]\w*)",
                text,
            )
        )

    def type_names(cpp_type: str) -> list[str]:
        cleaned = re.sub(r"<.*>", "", cpp_type)
        cleaned = re.sub(r"\{.*\}", "", cleaned)
        cleaned = re.sub(r"&|\*|\bconst\b|\bvolatile\b", "", cleaned).strip()
        cleaned = re.sub(r"\s+\w+\s*$", "", cleaned)
        cleaned = cleaned.split("=", 1)[0].strip()
        if not cleaned:
            return []
        return list(filter(None, re.split(r"[^\w:]+", cleaned)))

    for member in config.lexer_members + config.token_members:
        for required in type_names(member.cpp_type):
            if required and required not in declared and required not in builtin_types:
                raise RuleError(
                    member.line_no,
                    f"tipo inexistente em types.hpp: {member.cpp_type!r}",
                )


def write_generated(out_dir: Path, config: LexerConfig) -> list[Path]:
    written: list[Path] = []
    try:
        for name, content in generated_files(config):
            target = out_dir / name
            target.write_text(content, encoding="utf-8")
            written.append(target)
    except ValueError as exc:
        raise RuleError(1, str(exc)) from exc
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/frontend/lexer/lexer.rules")
    parser.add_argument("--out", default="build/src/frontend/lexer")
    parser.add_argument(
        "--types",
        default="src/frontend/lexer/types.hpp",
        help="user-owned C++ header containing required type declarations",
    )
    args = parser.parse_args()

    rules_path = Path(args.rules)
    types_path = Path(args.types)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2
    if not types_path.exists():
        print(f"types file not found: {types_path}", file=sys.stderr)
        return 2

    try:
        config = parse_rules(rules_path.read_text(encoding="utf-8"), rules_path)
        if config.token_members and config.on_token is None:
            raise RuleError(1, "[token-type] requer [actions] onToken")
        validate_types(config, types_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    out_path.mkdir(parents=True, exist_ok=True)
    try:
        written = write_generated(out_path, config)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
