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
    dedupe_preserve_order,
    gitignore_lines,
    join_logical_lines,
    parse_bool_flag,
    parse_hook as parse_hook_name,
    parse_quoted,
    parse_string_list,
    split_top_level,
    parse_typed_member,
    write_generated as write_generated_files,
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

# Map TokenKind name -> ordinal position in the TOKEN_KINDS list.
_TOKEN_ORDINAL = {name: i for i, name in enumerate(TOKEN_KINDS)}

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
class KeywordEntry:
    kind: str
    word: str
    length: int
    packed: int          # LE uint64_t
    tail: int            # 9th byte (0 for len <= 8)
    token_ordinal: int   # position in TOKEN_KINDS
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
    keyword_entries: list[KeywordEntry]
    lexer_members: list[Member]
    token_members: list[Member]
    on_lex: Hook | None
    off_lex: Hook | None
    on_token: Hook | None


def parse_hook(raw: str, line_no: int) -> Hook:
    return Hook(qualified_name=parse_hook_name(raw, line_no), line_no=line_no)


def parse_member(raw: str, line_no: int) -> Member:
    name, cpp_type, default = parse_typed_member(
        raw,
        line_no,
        "member",
        requires_default=True,
    )
    return Member(name=name, cpp_type=cpp_type, initializer=default, line_no=line_no)


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
        return TokenSpec(name, line_no, "list", strings=parse_string_list(inner, line_no))

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
            if kind not in _TOKEN_ORDINAL:
                raise RuleError(line_no, f"TokenKind desconhecido: {kind!r}")
            if rhs.startswith("[") and rhs.endswith("]"):
                values = parse_string_list(rhs[1:-1], line_no)
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

    # --- Validation: duplicates and ASCII ---
    seen: dict[str, tuple[str, int]] = {}
    for kind, word in keywords:
        if word in seen:
            prev_kind, prev_line = seen[word]
            raise RuleError(
                line_no,
                f"keyword duplicada {word!r}: "
                f"primeiro uso como {prev_kind!r} (linha {prev_line}), "
                f"agora como {kind!r}",
            )
        seen[word] = (kind, line_no)
        for ch in word:
            if ord(ch) >= 128:
                raise RuleError(
                    line_no,
                    f"keyword {word!r} contem caractere nao-ASCII {ch!r}",
                )

    # --- Build keyword entries (pre-compute packed values) ---
    keyword_entries: list[KeywordEntry] = []
    for kind, word in keywords:
        raw = word.encode("ascii")
        length = len(word)
        packed = 0
        for i_b, b in enumerate(raw[:8]):
            packed |= b << (i_b * 8)
        tail = raw[8] if length >= 9 else 0
        keyword_entries.append(
            KeywordEntry(
                kind=kind, word=word, length=length, packed=packed,
                tail=tail, token_ordinal=_TOKEN_ORDINAL[kind],
            )
        )

    return LexerConfig(
        tokens=tokens,
        keywords=keywords,
        keyword_entries=keyword_entries,
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
            raise RuleError(1, "perfect hash bucket overflow (>16 keywords per bucket)")

    seeds = [0] * BUCKET_COUNT
    table = [-1] * TABLE_SIZE
    # Sort buckets: largest first, tie-break on lowest bucket index for determinism.
    bucket_order = sorted(
        range(BUCKET_COUNT), key=lambda bi: (-len(buckets[bi]), bi)
    )

    for bucket_index in bucket_order:
        bucket = buckets[bucket_index]
        if not bucket:
            continue
        placed = False
        for seed in range(256):
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
            raise RuleError(1, "perfect hash placement failed; increase TableSize")
    return seeds, table


def format_int_array(values: list[int], per_line: int = 16) -> list[str]:
    lines: list[str] = []
    for start in range(0, len(values), per_line):
        lines.append(", ".join(str(value) for value in values[start : start + per_line]))
    return lines


def make_bool_table(enabled: set[int]) -> list[int]:
    return [1 if index in enabled else 0 for index in range(256)]


def make_keyword_header(
    keywords: list[tuple[str, str]], entries: list[KeywordEntry],
) -> str:
    seeds, table = build_perfect_hash(keywords)
    lines = [
        "#pragma once",
        '#include "lexer.hpp"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
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
        f"constexpr size_t BUCKET_COUNT = {BUCKET_COUNT};",
        f"constexpr size_t TABLE_SIZE = {TABLE_SIZE};",
        "",
        "struct KeywordMeta {",
        "    uint8_t length;",
        "    uint8_t token_kind;",
        "    uint64_t packed;",
        "    uint8_t tail;",
        "};",
        "",
        f"constexpr std::array<KeywordMeta, {len(entries)}> keyword_meta = {{{{",
    ]
    for entry in entries:
        lines.append(
            f"    {{{entry.length}, {entry.token_ordinal}, "
            f"UINT64_C({entry.packed:#018x}), {entry.tail}}},"
        )
    lines.append("}};")
    lines.append("")

    lines.append(f"constexpr std::array<int16_t, {TABLE_SIZE}> hash_table = {{")
    lines.extend(f"    {line}," for line in format_int_array(table))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::array<uint8_t, {BUCKET_COUNT}> bucket_seed = {{")
    lines.extend(f"    {line}," for line in format_int_array(seeds))
    lines.append("};")
    lines.append("")

    # Static verification that all keywords are placed correctly.
    lines.extend(
        [
            "constexpr bool allKeywordsPlaced() noexcept {",
            f"    constexpr std::string_view spellings[{len(entries)}] = {{",
        ]
    )
    for entry in entries:
        lines.append(f'        "{entry.word}",')
    lines.extend(
        [
            "    };",
            "    for (size_t i = 0; i < keyword_meta.size(); ++i) {",
            "        const uint64_t h = hash64(spellings[i]);",
            "        const size_t b = h % BUCKET_COUNT;",
            "        const size_t slot = mix64(h ^ bucket_seed[b]) % TABLE_SIZE;",
            "        const int16_t id = hash_table[slot];",
            "        if (id < 0 || static_cast<size_t>(id) != i)",
            "            return false;",
            "    }",
            "    return true;",
            "}",
            'static_assert(allKeywordsPlaced(), "not all keywords placed in perfect hash");',
            "",
            "} // namespace detail",
            "",
            "inline TokenKind lookupKeyword(std::string_view sv) noexcept {",
            "    using namespace detail;",
            "    if (sv.empty()) return TokenKind::Identifier;",
            "    const uint64_t h = hash64(sv);",
            "    const size_t b = h % BUCKET_COUNT;",
            "    const size_t slot = mix64(h ^ bucket_seed[b]) % TABLE_SIZE;",
            "    const int16_t id = hash_table[slot];",
            "    if (id < 0) return TokenKind::Identifier;",
            "    const auto &m = keyword_meta[static_cast<size_t>(id)];",
            "    if (m.length != sv.size()) return TokenKind::Identifier;",
            "    if (m.length < 8) {",
            "        // Copy exactly m.length bytes into a zeroed uint64_t.",
            "        // m.length is [1,7]; sv.data() has >= m.length bytes",
            "        // because sv.size() == m.length.",
            "        uint64_t word = 0;",
            "        __builtin_memcpy(&word, sv.data(), m.length);",
            "        if (word == m.packed)",
            "            return static_cast<TokenKind>(m.token_kind);",
            "    } else if (m.length == 8) {",
            "        uint64_t word = 0;",
            "        __builtin_memcpy(&word, sv.data(), sizeof(word));",
            "        if (word == m.packed)",
            "            return static_cast<TokenKind>(m.token_kind);",
            "    } else {",
            "        // length == 9: load first 8 bytes + compare 9th byte.",
            "        uint64_t word = 0;",
            "        __builtin_memcpy(&word, sv.data(), sizeof(word));",
            "        if (word == m.packed",
            "            && static_cast<unsigned char>(sv[8]) == m.tail)",
            "            return static_cast<TokenKind>(m.token_kind);",
            "    }",
            "    return TokenKind::Identifier;",
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
        "#include <cstdio>",
        "#include <span>",
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
            "    std::vector<Token> tokens;",
            "    const Token *src = nullptr;",
            "    uint32_t len = 0;",
            "    uint32_t offset = 0;",
            "",
            "    TokenStream() = default;",
            "    explicit TokenStream(std::vector<Token> tokens_)",
            "        : tokens(std::move(tokens_)),",
            "          src(tokens.data()), len(static_cast<uint32_t>(tokens.size())) {}",
            "    explicit TokenStream(std::span<const Token> tokens_)",
            "        : tokens(tokens_.begin(), tokens_.end()),",
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
            "    template <typename... Args>",
            "    bool matchAll(Args... args) noexcept {",
            "        const size_t saved = offset;",
            "        if ((match(args) && ...)) return true;",
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
            "        std::string_view source",
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
            "    std::string_view source",
            ");",
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
            pair = parse_string_list(multi_raw, comments_spec.line_no)
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
        "#include <cstring>",
        "#include <utility>",
        "",
        "namespace generated_lexer {",
        "",
        "namespace detail {",
        "",
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
            "inline bool table_contains(",
            "    const std::array<uint8_t, 256> &table, unsigned char ch",
            ") noexcept {",
            "    return table[ch] != 0;",
            "}",
            "",
            "inline size_t consume_table_run(",
            "    const char *text, size_t size, size_t offset,",
            "    const std::array<uint8_t, 256> &table",
            ") noexcept {",
            "    if (table_contains(table, static_cast<unsigned char>(text[offset])))",
            "        ++offset;",
            "    while (offset < size &&",
            "           table_contains(table, static_cast<unsigned char>(text[offset]))) {",
            "        if (offset + kRunChunkBytes <= size &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 1])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 2])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 3])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 4])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 5])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 6])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 7])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 8])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 9])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 10])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 11])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 12])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 13])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 14])) &&",
            "            table_contains(table, static_cast<unsigned char>(text[offset + 15]))) {",
            "            offset += kRunChunkBytes;",
            "        } else {",
            "            ++offset;",
            "        }",
            "    }",
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
            "inline bool is_ascii_alpha(unsigned char c) noexcept {",
            "    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');",
            "}",
            "",
            "inline bool is_identifier_start(unsigned char c) noexcept {",
            "    return is_ascii_alpha(c) || c == '_';",
            "}",
            "",
            "inline size_t consume_decimal_run(",
            "    const char *text, size_t size, size_t offset",
            ") noexcept {",
            "    while (offset < size) {",
            "        const unsigned char c = static_cast<unsigned char>(text[offset]);",
            "        if (c < '0' || c > '9')",
            "            return offset;",
            "        if (offset + kRunChunkBytes <= size) {",
            "            bool chunk = true;",
            "            for (size_t i = 1; i < kRunChunkBytes; ++i) {",
            "                const unsigned char d =",
            "                    static_cast<unsigned char>(text[offset + i]);",
            "                chunk &= d >= '0' && d <= '9';",
            "            }",
            "            if (chunk) {",
            "                offset += kRunChunkBytes;",
            "                continue;",
            "            }",
            "        }",
            "        ++offset;",
            "    }",
            "    return offset;",
            "}",
            "",
            "inline bool consume_decimal_digit_run(",
            "    const char *text, size_t size, size_t &offset, bool allow_underscore",
            ") noexcept {",
            "    const size_t start = offset;",
            "    offset = consume_decimal_run(text, size, offset);",
            "    bool saw_digit = offset > start;",
            "    bool prev_underscore = false;",
            "    bool valid = true;",
            "    while (offset < size) {",
            "        const unsigned char ch = static_cast<unsigned char>(text[offset]);",
            "        if (ch >= '0' && ch <= '9') {",
            "            saw_digit = true;",
            "            prev_underscore = false;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        if (allow_underscore && ch == '_') {",
            "            if (!saw_digit || prev_underscore) {",
            "                valid = false;",
            "                while (offset < size) {",
            "                    const unsigned char tail = static_cast<unsigned char>(text[offset]);",
            "                    if (!(tail >= '0' && tail <= '9') && tail != '_')",
            "                        break;",
            "                    ++offset;",
            "                }",
            "                break;",
            "            }",
            "            prev_underscore = true;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        break;",
            "    }",
            "    if (valid && offset > start)",
            "        saw_digit = true;",
            "    return valid && saw_digit && !prev_underscore;",
            "}",
            "",
            "template <typename Table>",
            "inline bool consume_number_digit_run(",
            "    const char *text, size_t size, size_t &offset, bool allow_underscore,",
            "    const Table &digits",
            ") noexcept {",
            "    const size_t start = offset;",
            "    offset = consume_table_run(text, size, offset, digits);",
            "    bool saw_digit = offset > start;",
            "    bool prev_underscore = false;",
            "    bool valid = true;",
            "    while (offset < size) {",
            "        const unsigned char ch = static_cast<unsigned char>(text[offset]);",
            "        if (table_contains(digits, ch)) {",
            "            saw_digit = true;",
            "            prev_underscore = false;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        if (allow_underscore && ch == '_') {",
            "            if (!saw_digit || prev_underscore) {",
            "                valid = false;",
            "                while (offset < size) {",
            "                    const unsigned char tail = static_cast<unsigned char>(text[offset]);",
            "                    const bool tail_digit = table_contains(digits, tail);",
            "                    if (!tail_digit && tail != '_')",
            "                        break;",
            "                    ++offset;",
            "                }",
            "                break;",
            "            }",
            "            prev_underscore = true;",
            "            ++offset;",
            "            continue;",
            "        }",
            "        break;",
            "    }",
            "    if (valid && offset > start)",
            "        saw_digit = true;",
            "    return valid && saw_digit && !prev_underscore;",
            "}",
            "",
            "} // namespace detail",
            "",
        ]
    )
    if config.on_lex or config.off_lex or config.on_token:
        lines.insert(2, '#include "actions.hpp"')
        lines.insert(3, "")

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
            "namespace detail {",
            "",
            "template <typename LexerT>",
            "inline TokenStream run_lexer(LexerT &lexer, std::string_view source)",
            "{",
            "    std::vector<Token> tokens;",
            "    tokens.reserve(source.size() / 4 + 8);",
            "",
            "    const char *text = source.data();",
            "    const size_t size = source.size();",
            "",
            "    const auto span = [](size_t start, size_t end) {",
            "        return Span{static_cast<uint32_t>(start), static_cast<uint32_t>(end)};",
            "    };",
            "    size_t offset = 0;",
        ]
    )
    if config.on_token:
        lines.extend(
            [
                "    const auto emit = [&](TokenKind kind, Span s, char punc = 0) {",
                "        tokens.emplace_back(s, kind, punc);",
                f"        {config.on_token.call('lexer, std::as_const(tokens.back()), source.substr(s.start, s.end - s.start)')}",
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
    if config.on_lex:
        lines.extend(["", f"    {config.on_lex.call('lexer, source')}"])
    lines.extend(
        [
            "",
            "    while (offset < size) {",
            "        offset = detail::consume_ascii_space(text, size, offset);",
            "        if (offset >= size)",
            "            break;",
            "",
            "        const size_t before = offset;",
            "        const char c = text[offset];",
            "",
        ]
    )

    if single_comment:
        lines.extend(
            [
                f"        if ({' && '.join(f'offset + {index + 1} <= size && text[offset + {index}] == {cpp_char(ch)}' for index, ch in enumerate(single_comment))}) {{",
                f"            offset += {len(single_comment)};",
                "            while (offset < size && text[offset] != '\\n')",
                "                ++offset;",
                "            emit(TokenKind::Comments, span(before, offset));",
                "            continue;",
                "        }",
                "",
            ]
        )
    if multi_open and multi_close:
        iter_item = iter(multi_close)
        first_close = next(iter_item, "")
        rest_close = list(iter_item)
        lines.extend(
            [
                f"        if ({' && '.join(f'offset + {index + 1} <= size && text[offset + {index}] == {cpp_char(ch)}' for index, ch in enumerate(multi_open))}) {{",
                f"            offset += {len(multi_open)};",
                "            bool closed = false;",
                f"            const void *search = text + offset;",
                f"            size_t search_size = size - offset;",
                "            while (search_size > 0) {",
                f"                const void *hit = std::memchr(search, {cpp_char(first_close)}, search_size);",
                "                if (hit == nullptr)",
                "                    break;",
                f"                const size_t found = static_cast<size_t>(static_cast<const char *>(hit) - text);",
                f"                const bool full = {'true' if len(rest_close) == 0 else 'false'}",
                f"                    ? true",
                f"                    : {' && '.join(f'found + {index + 1} < size && text[found + {index + 1}] == {cpp_char(ch)}' for index, ch in enumerate(rest_close))};",
                "                if (full) {",
                f"                    offset = found + {len(multi_close)};",
                "                    closed = true;",
                "                    break;",
                "                }",
                f"                search = static_cast<const char *>(hit) + 1;",
                f"                search_size = size - found - 1;",
                "            }",
                "            if (!closed)",
                "                offset = size;",
                "            emit(TokenKind::Comments, span(before, offset));",
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
            "            ++offset;",
        ]
    )
    if escape_sequences:
        lines.extend(
            [
                "            while (offset + 8 <= size) {",
                "                uint64_t chunk = 0;",
                "                std::memcpy(&chunk, text + offset, 8);",
                "                const uint8_t q = static_cast<uint8_t>(c);",
                "                const uint64_t quote_diff = chunk ^ "
                "                    (static_cast<uint64_t>(q) * UINT64_C(0x0101010101010101));",
                "                const uint64_t slash_diff = chunk ^ UINT64_C(0x5c5c5c5c5c5c5c5c);",
                "                const uint64_t quote_hit = (quote_diff - UINT64_C(0x0101010101010101)) & (~quote_diff) & UINT64_C(0x8080808080808080);",
                "                const uint64_t slash_hit = (slash_diff - UINT64_C(0x0101010101010101)) & (~slash_diff) & UINT64_C(0x8080808080808080);",
                "                const uint64_t control_hit = (chunk - UINT64_C(0x2020202020202020)) & UINT64_C(0x8080808080808080);",
                "                const uint64_t non_ascii = chunk & UINT64_C(0x8080808080808080);",
                "                const uint64_t special = quote_hit | slash_hit | control_hit | non_ascii;",
                "                if (special != 0)",
                "                    break;",
                "                offset += 8;",
                "            }",
                "            while (offset < size) {",
                "                if (text[offset] == '\\n' || text[offset] == '\\r') {",
                "                    valid = false;",
                "                    break;",
                "                }",
                "                if (text[offset] == '\\\\') {",
                "                    if (offset + 1 >= size) {",
                "                        ++offset;",
                "                        valid = false;",
                "                        break;",
                "                    }",
                "                    switch (text[offset + 1]) {",
                "                    case '\\\\':",
                "                    case '\"':",
                "                    case '\\'':",
                "                    case 'n':",
                "                    case 'r':",
                "                    case 't':",
                "                    case '0':",
                "                        offset += 2;",
                "                        continue;",
                "                    default:",
                "                        valid = false;",
                "                        offset += 2;",
                "                        continue;",
                "                    }",
                "                }",
                "                if (text[offset] == c) {",
                "                    ++offset;",
                "                    terminated = true;",
                "                    break;",
                "                }",
                "                ++offset;",
                "            }",
            ]
        )
    else:
        lines.extend(
            [
                "            while (offset < size) {",
                "                if (text[offset] == '\\n' || text[offset] == '\\r') {",
                "                    valid = false;",
                "                    break;",
                "                }",
                "                if (text[offset] == c) {",
                "                    ++offset;",
                "                    terminated = true;",
                "                    break;",
                "                }",
                "                ++offset;",
                "            }",
            ]
        )
    lines.extend(
        [
            "            if (!terminated)",
            "                valid = false;",
            "            emit(valid ? TokenKind::LitVal : TokenKind::Unknown,",
            "                 span(before, offset));",
            "            continue;",
            "        }",
            "",
            "        if (c >= '0' && c <= '9') {",
        ]
    )
    for prefix, table_name in number_prefixes:
        prefix_checks = " && ".join(
            f"offset + {index + 1} <= size && text[offset + {index}] == {cpp_char(ch)}"
            for index, ch in enumerate(prefix)
        )
        lines.extend(
            [
                f"            if ({prefix_checks}) {{",
            f"                offset += {len(prefix)};",
                f"                const bool valid = detail::consume_number_digit_run(",
                f"                    text, size, offset, true, detail::{table_name}",
                "                );",
                "                emit(valid ? TokenKind::LitVal : TokenKind::Unknown,",
                "                     span(before, offset));",
                "                continue;",
                "            }",
            ]
        )
    lines.extend(
        [
            "            const bool integer_valid = detail::consume_decimal_digit_run(",
            f"                text, size, offset, {'true' if decimal_underscore else 'false'}",
            "            );",
            "            if (offset < size && text[offset] == '.' &&",
            "                offset + 1 < size &&",
            "                detail::table_contains(",
            "                    detail::decimal_digit_table,",
            "                    static_cast<unsigned char>(text[offset + 1])",
            "                )) {",
            "                ++offset;",
            "                const bool fraction_valid = detail::consume_decimal_digit_run(",
            f"                    text, size, offset, {'true' if decimal_underscore else 'false'}",
            "                );",
            "                emit(integer_valid && fraction_valid ? TokenKind::LitVal",
            "                                                  : TokenKind::Unknown,",
            "                     span(before, offset));",
            "                continue;",
            "            }",
            "            emit(integer_valid ? TokenKind::LitVal : TokenKind::Unknown,",
            "                 span(before, offset));",
            "            continue;",
            "        }",
            "",
            "        if (detail::is_identifier_start(static_cast<unsigned char>(c))) {",
            "            ++offset;",
            "            offset = detail::consume_identifier_tail(text, size, offset);",
            "            const std::string_view word(text + before, offset - before);",
            "            emit(lookupKeyword(word), span(before, offset));",
            "            continue;",
            "        }",
            "",
        ]
    )
    if compound:
        grouped: dict[str, list[tuple[int, str]]] = {}
        for index, op in enumerate(compound):
            grouped.setdefault(op[0], []).append((index, op))
        lines.append("        switch (static_cast<unsigned char>(c)) {")
        for first_char, ops in grouped.items():
            lines.append(f"        case {cpp_char(first_char)}:")
            for index, op in ops:
                op_checks = " && ".join(
                    f"offset + {i + 1} <= size && text[offset + {i}] == {cpp_char(ch)}"
                    for i, ch in enumerate(op)
                )
                lines.extend(
                    [
                        f"            if ({op_checks}) {{",
                        f"                offset += {len(op)};",
                        "                emit(TokenKind::Operators, span(before, offset));",
                        "                continue;",
                        "            }",
                    ]
                )
            lines.append("            break;")
        lines.extend(["        }", ""])

    if op_chars:
        lines.extend(["        switch (static_cast<unsigned char>(c)) {"])
        for ch in dedupe_preserve_order(list(op_chars)):
            lines.append(f"        case {cpp_char(ch)}:")
        lines.extend(
            [
            "            ++offset;",
            "            emit(TokenKind::Operators, span(before, offset), c);",
                "            continue;",
                "        default:",
                "            break;",
                "        }",
                "",
            ]
        )

    if punc_chars:
        lines.extend(["        switch (static_cast<unsigned char>(c)) {"])
        for ch in dedupe_preserve_order(list(punc_chars)):
            lines.append(f"        case {cpp_char(ch)}:")
        lines.extend(
            [
            "            ++offset;",
            "            emit(TokenKind::Punctuation, span(before, offset), c);",
                "            continue;",
                "        default:",
                "            break;",
                "        }",
                "",
            ]
        )

    lines.extend(
        [
            "        ++offset;",
            "        emit(TokenKind::Unknown, span(before, offset));",
            "    }",
            "",
            "    emit(TokenKind::End, span(size, size));",
        ]
    )
    if config.off_lex:
        lines.append(f"    {config.off_lex.call('lexer, source, tokens')}")
    lines.extend(
        [
            "    return TokenStream(std::move(tokens));",
            "}",
            "",
            "} // namespace detail",
            "",
            "TokenStream Lexer::run(std::string_view source) {",
            "    source_ = source;",
            "    offset_ = 0;",
            "    return detail::run_lexer(*this, source);",
            "}",
            "",
            "TokenStream tokenize(std::string_view source) {",
            "    return Lexer().run(source);",
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
    return gitignore_lines([
        "lexer.hpp",
        "lexer.cpp",
        "actions.hpp",
        "keyword-table.hpp",
    ])


def generated_files(config: LexerConfig) -> list[tuple[str, str]]:
    return [
        ("lexer.hpp", make_lexer_header(config)),
        ("lexer.cpp", make_lexer_source(config)),
        ("actions.hpp", make_actions_header(config)),
        ("keyword-table.hpp",
         make_keyword_header(config.keywords, config.keyword_entries)),
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
    try:
        return write_generated_files(out_dir, generated_files(config))
    except ValueError as exc:
        raise RuleError(1, str(exc)) from exc


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
