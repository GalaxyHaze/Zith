#!/usr/bin/env python3
"""Generate lexer.hpp, lexer.cpp, actions.hpp, keyword-table.hpp and .gitignore
from lexer.rules."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


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


class RuleError(ValueError):
    def __init__(self, line_no: int, message: str) -> None:
        super().__init__(message)
        self.line_no = line_no
        self.message = message

    def render(self, path: Path) -> str:
        return f"{path}:{self.line_no}: {self.message}"


def strip_comment(line: str) -> str:
    quote: str | None = None
    escaped = False
    out: list[str] = []
    for ch in line:
        if quote:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            continue
        if ch in {"'", '"'}:
            quote = ch
            out.append(ch)
            continue
        if ch == "#":
            break
        out.append(ch)
    return "".join(out).strip()


def cpp_string(value: str) -> str:
        return json.dumps(value, ensure_ascii=True)


def cpp_char(value: str) -> str:
    if len(value) != 1:
        raise ValueError(f"expected one char, got {value!r}")
    escaped = json.dumps(value, ensure_ascii=True)[1:-1]
    if escaped == "\\":
        return "'\\\\'"
    return f"'{escaped}'"


def parse_quoted(raw: str, line_no: int) -> str:
    raw = raw.strip()
    if not (
        (raw.startswith('"') and raw.endswith('"'))
        or (raw.startswith("'") and raw.endswith("'"))
    ):
        raise RuleError(line_no, f"string esperada: {raw!r}")
    body = raw[1:-1]
    try:
        return json.loads(f'"{body}"' if raw.startswith('"') else f'"{body}"')
    except (ValueError, SyntaxError) as exc:
        raise RuleError(line_no, f"string invalida: {raw!r}") from exc


def split_top_level(body: str, line_no: int) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    quote: str | None = None
    escaped = False
    for index, ch in enumerate(body):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            continue
        if ch in {"'", '"'}:
            quote = ch
        elif ch in "[{(":
            depth += 1
        elif ch in "]})":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(body[start:index].strip())
            start = index + 1
    if quote or depth != 0:
        raise RuleError(line_no, "lista/string nao terminada")
    parts.append(body[start:].strip())
    return parts


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
    def name(self) -> str:
        return self.qualified_name.rsplit("::", 1)[-1]

    @property
    def declaration(self) -> str:
        return f"void {self.name}();"

    @property
    def call(self) -> str:
        return f"{self.qualified_name}();"


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

    logical: list[tuple[int, str]] = []
    pending: str | None = None
    pending_line = 0

    def flush_pending() -> None:
        nonlocal pending
        if pending is not None:
            logical.append((pending_line, pending))
            pending = None

    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue
        if re.fullmatch(r"\[([^\]]+)\]", line):
            flush_pending()
            logical.append((line_no, line))
            continue
        if pending is None:
            pending = line
            pending_line = line_no

            if (
                pending.count("[") == pending.count("]")
                and pending.count('"') % 2 == 0
                and pending.count("'") % 2 == 0
            ):
                flush_pending()
        else:
            pending += " " + line
            if (
                pending.count("[") == pending.count("]")
                and pending.count('"') % 2 == 0
                and pending.count("'") % 2 == 0
            ):
                flush_pending()
    flush_pending()

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
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "#include <vector>",
        "",
        "namespace generated_lexer {",
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
            "    const Token *src = nullptr;",
            "    uint32_t len = 0;",
            "    uint32_t offset = 0;",
            "    const char *fileBase = nullptr;",
            "",
            "    [[nodiscard]] std::string_view lexeme(const Token &t) const noexcept {",
            "        if (!fileBase) return {};",
            "        return {fileBase + t.span.start,",
            "                static_cast<size_t>(t.span.end - t.span.start)};",
            "    }",
            "    [[nodiscard]] const Token &peek() const noexcept;",
            "    void advance() noexcept;",
            "};",
            "",
            "class Lexer {",
            "public:",
            "    Lexer();",
            "    [[nodiscard]] std::vector<Token> run(std::string_view source);",
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
            "[[nodiscard]] std::vector<Token> tokenize(std::string_view source);",
            "const char *tokenKindName(TokenKind kind) noexcept;",
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
    skip_exprs = [f"c == {cpp_char(ch)}" for ch in skip_chars]
    skip_if = " || ".join(skip_exprs) if skip_exprs else "false"

    punc_spec = tokens.get("punc")
    punc_chars = punc_spec.value if punc_spec and punc_spec.kind == "string" else ""
    op_spec = tokens.get("operators")
    op_chars = op_spec.value if op_spec and op_spec.kind == "string" else ""
    arrow_spec = tokens.get("arrow")
    arrow = arrow_spec.value if arrow_spec and arrow_spec.kind == "string" else "->"
    compound_spec = tokens.get("compound")
    compound = compound_spec.strings if compound_spec else []

    string_spec = tokens.get("string")
    string_opts = string_spec.options if string_spec else {}
    escape_sequences = string_opts.get("escape-sequence", "true").lower() == "true"

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
    for name, digit_line in (
        ("hexadecimal", "is_hex_digit(text[offset_])"),
        ("octal", "(text[offset_] >= '0' && text[offset_] <= '7')"),
        ("binary", "(text[offset_] == '0' || text[offset_] == '1')"),
    ):
        spec = tokens.get(name)
        if spec and spec.enabled:
            prefix = spec.option_string("prefix", spec.line_no)
            number_prefixes.append((prefix, digit_line))

    lines = [
        '#include "lexer.hpp"',
        '#include "keyword-table.hpp"',
        '#include "actions.hpp"',
        "",
        "#include <utility>",
        "",
        "namespace generated_lexer {",
        "",
        "static bool is_ascii_space(char c) noexcept {",
        f"    return {skip_if};",
        "}",
        "",
        "static bool is_ascii_alpha(char c) noexcept {",
        "    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');",
        "}",
        "",
        "static bool is_ascii_alnum(char c) noexcept {",
        "    return is_ascii_alpha(c) || (c >= '0' && c <= '9');",
        "}",
        "",
        "static bool is_hex_digit(char c) noexcept {",
        "    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||",
        "           (c >= 'A' && c <= 'F');",
        "}",
        "",
        "static bool prefix_matches(std::string_view rest, std::string_view prefix) noexcept {",
        "    return rest.size() >= prefix.size() && rest.substr(0, prefix.size()) == prefix;",
        "}",
        "",
    ]

    if punc_chars:
        lines.extend(
            [
                "constexpr std::array<std::string_view, "
                f"{len(punc_chars)}> punctuation_set = {{",
                f"    {', '.join(cpp_string(ch) for ch in punc_chars)},",
                "};",
                "",
            ]
        )
    if op_chars:
        lines.extend(
            [
                f"constexpr std::array<std::string_view, {len(op_chars)}> operator_set = {{",
                f"    {', '.join(cpp_string(ch) for ch in op_chars)},",
                "};",
                "",
            ]
        )
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
            "std::vector<Token> Lexer::run(std::string_view source) {",
            "    source_ = source;",
            "    offset_ = 0;",
            "    std::vector<Token> tokens;",
            "",
        ]
    )
    if config.on_lex:
        lines.append(f"    {config.on_lex.call}")
        lines.append("")
    lines.extend(
        [
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
                f"        {config.on_token.call}",
                "        tokens.emplace_back(s, kind, punc);",
                "    };",
            ]
        )
    else:
        lines.extend(
            [
                "    const auto emit = [&](TokenKind kind, Span s, char punc = 0) {",
                "        tokens.emplace_back(s, kind, punc);",
                "    };",
            ]
        )
    lines.extend(
        [
            "",
            "    while (offset_ < size) {",
            "        const size_t before = offset_;",
            "        const char c = text[offset_];",
            "",
            "        if (is_ascii_space(c)) {",
            "            ++offset_;",
            "            continue;",
            "        }",
            "",
            "        const std::string_view rest(text + offset_, size - offset_);",
        ]
    )

    if single_comment:
        lines.extend(
            [
                f"        if (prefix_matches(rest, {cpp_string(single_comment)})) {{",
                f"            offset_ += {len(single_comment)};",
                "            while (offset_ < size && text[offset_] != '\\n') ++offset_;",
                "            emit(TokenKind::Comments, span(before, offset_));",
                "            continue;",
                "        }",
                "",
            ]
        )
    if multi_open and multi_close:
        lines.extend(
            [
                f"        if (prefix_matches(rest, {cpp_string(multi_open)})) {{",
                f"            offset_ += {len(multi_open)};",
                f"            const size_t close_at = rest.find({cpp_string(multi_close)}, {len(multi_open)});",
                "            if (close_at == std::string_view::npos)",
                "                offset_ = size;",
                "            else",
                f"                offset_ = close_at + {len(multi_close)};",
                "            emit(TokenKind::Comments, span(before, offset_));",
                "            continue;",
                "        }",
                "",
            ]
        )

    lines.extend(
        [
            "        if (c == '\"' || c == '\\'') {",
            "            ++offset_;",
        ]
    )
    if escape_sequences:
        lines.extend(
            [
                "            while (offset_ < size) {",
                "                if (text[offset_] == '\\\\') {",
                "                    offset_ += 2;",
                "                    continue;",
                "                }",
                "                if (text[offset_] == c) { ++offset_; break; }",
                "                ++offset_;",
                "            }",
            ]
        )
    else:
        lines.extend(
            [
                "            while (offset_ < size && text[offset_] != c) ++offset_;",
                "            if (offset_ < size) ++offset_;",
            ]
        )
    lines.extend(
        [
            "            emit(TokenKind::LitVal, span(before, offset_));",
            "            continue;",
            "        }",
            "",
            "        if (c >= '0' && c <= '9') {",
        ]
    )
    for prefix, digit_condition in number_prefixes:
        lines.extend(
            [
                f"            if (prefix_matches(rest, {cpp_string(prefix)})) {{",
                f"                offset_ += {len(prefix)};",
                f"                while (offset_ < size && {digit_condition}) ++offset_;",
                "                emit(TokenKind::LitVal, span(before, offset_));",
                "                continue;",
                "            }",
            ]
        )
    lines.extend(
        [
            "            while (offset_ < size && text[offset_] >= '0' && "
            "text[offset_] <= '9') ++offset_;",
            "            if (offset_ < size && text[offset_] == '.' &&",
            "                offset_ + 1 < size && text[offset_ + 1] >= '0' &&",
            "                text[offset_ + 1] <= '9') {",
            "                ++offset_;",
            "                while (offset_ < size && text[offset_] >= '0' && "
            "text[offset_] <= '9') ++offset_;",
            "            }",
            "            emit(TokenKind::LitVal, span(before, offset_));",
            "            continue;",
            "        }",
            "",
            "        if (is_ascii_alpha(c) || c == '_') {",
            "            while (offset_ < size &&",
            "                   (is_ascii_alnum(text[offset_]) || text[offset_] == '_'))",
            "                ++offset_;",
            "            const std::string_view word(text + before, offset_ - before);",
            "            emit(lookupKeyword(word), span(before, offset_));",
            "            continue;",
            "        }",
            "",
        ]
    )
    if arrow:
        lines.extend(
            [
                f"        if (prefix_matches(rest, {cpp_string(arrow)})) {{",
                f"            offset_ += {len(arrow)};",
                "            emit(TokenKind::Operators, span(before, offset_));",
                "            continue;",
                "        }",
                "",
            ]
        )
    if compound:
        lines.extend(
            [
                "        for (const std::string_view op : compound_set) {",
                "            if (prefix_matches(rest, op)) {",
                "                offset_ += op.size();",
                "                emit(TokenKind::Operators, span(before, offset_));",
                "                goto next_char;",
                "            }",
                "        }",
                "",
            ]
        )
    if op_chars:
        lines.extend(
            [
                "        for (const std::string_view op : operator_set) {",
                "            if (c == op[0]) {",
                "                ++offset_;",
                "                emit(TokenKind::Operators, span(before, offset_), c);",
                "                goto next_char;",
                "            }",
                "        }",
                "",
            ]
        )
    if punc_chars:
        lines.extend(
            [
                "        for (const std::string_view p : punctuation_set) {",
                "            if (c == p[0]) {",
                "                ++offset_;",
                "                emit(TokenKind::Punctuation, span(before, offset_), c);",
                "                goto next_char;",
                "            }",
                "        }",
                "",
            ]
        )
    lines.extend(
        [
            "        ++offset_;",
            "        emit(TokenKind::Unknown, span(before, offset_));",
            "next_char:",
            "        ;",
            "    }",
            "",
            "    emit(TokenKind::End, span(size, size));",
        ]
    )
    if config.off_lex:
        lines.append(f"    {config.off_lex.call}")
    lines.extend(
        [
            "    return tokens;",
            "}",
            "",
            "std::vector<Token> tokenize(std::string_view source) {",
            "    Lexer lexer;",
            "    return lexer.run(source);",
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
            "} // namespace generated_lexer",
            "",
        ]
    )
    return "\n".join(lines)


def make_actions_header(config: LexerConfig) -> str:
    hooks = [hook for hook in (config.on_lex, config.off_lex, config.on_token) if hook]
    if not hooks:
        return "#pragma once\n"
    lines = ["#pragma once", "", "namespace generated_lexer {"]
    for hook in hooks:
        lines.append(f"{hook.declaration}")
    lines.append("} // namespace generated_lexer")
    return "\n".join(lines) + "\n"


def make_gitignore() -> str:
    return "lexer.hpp\nlexer.cpp\nactions.hpp\nkeyword-table.hpp\n"


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
    parser.add_argument("rules", nargs="?", default="src/lexer/lexer.rules")
    parser.add_argument("--out", default="src/lexer")
    parser.add_argument(
        "--types",
        default="src/lexer/types.hpp",
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
