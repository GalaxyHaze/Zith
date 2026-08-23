#!/usr/bin/env python3
"""Render Zith source as a visible ASCII-highlighted version.

The compiler has its own terminal colors, but many places that display code
(markdown viewers, tickets, plain text editors) have no Zith highlighter.
This script tokensizes Zith source and re-emits it with ASCII markers so the
category of each token is visible without color.

Usage:
    python3 scripts/zith-ascii.py --string 'fn main() { printf("hi"); }'
    python3 scripts/zith-ascii.py examples/hello-world.zith
    cat examples/arrays.zith | python3 scripts/zith-ascii.py
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


KIND_KEYWORD = "kw"
KIND_TYPE = "ty"
KIND_LITERAL = "lit"
KIND_STRING = "str"
KIND_NUMBER = "num"
KIND_COMMENT = "cmt"
KIND_DOC = "doc"
KIND_ANNOTATION = "ann"
KIND_ATTRIBUTE = "att"
KIND_OPERATOR = "op"
KIND_PUNCTUATION = "punc"
KIND_IDENTIFIER = "id"
KIND_UNKNOWN = "unk"


KEYWORDS = {
    "alias",
    "as",
    "async",
    "await",
    "belong",
    "break",
    "catch",
    "component",
    "const",
    "context",
    "continue",
    "default",
    "dock",
    "drop",
    "dyn",
    "eager",
    "else",
    "enum",
    "export",
    "extends",
    "extern",
    "fail",
    "flow",
    "fn",
    "for",
    "from",
    "global",
    "if",
    "implement",
    "import",
    "in",
    "infix",
    "interface",
    "is",
    "jump",
    "lend",
    "let",
    "macro",
    "marker",
    "match",
    "mod",
    "must",
    "mut",
    "nop",
    "not",
    "operator",
    "or",
    "prefix",
    "pub",
    "raw",
    "require",
    "requires",
    "return",
    "share",
    "spawn",
    "stackful",
    "struct",
    "suffix",
    "throw",
    "token",
    "trait",
    "type",
    "union",
    "unique",
    "unsafe",
    "use",
    "var",
    "view",
    "when",
    "while",
    "with",
    "word",
    "xor",
    "yield",
}

TYPES = {
    "bool",
    "char",
    "f32",
    "f64",
    "i128",
    "i16",
    "i32",
    "i64",
    "i8",
    "invalid",
    "never",
    "null",
    "true",
    "false",
    "u128",
    "u16",
    "u32",
    "u64",
    "u8",
    "unknown",
    "void",
}

LITERALS = {
    "null",
    "true",
    "false",
}

KEYWORDS -= TYPES
TYPES -= LITERALS


OPERATORS = (
    "<<=",
    ">>=",
    "<<",
    ">>",
    "==",
    "!=",
    "<=",
    ">=",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "&=",
    "|=",
    "^=",
    "&.",
    "|.",
    "^.",
    "->",
    "..",
    "=>",
    "~>",
    "::",
    "+",
    "-",
    "*",
    "/",
    "%",
    "=",
    "<",
    ">",
    "!",
    "&",
    "|",
    "^",
    "~",
    "?",
)

PUNCTUATION = set("()[]{}:;,.\"'`")


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    start: int
    end: int


class Tokenizer:
    def __init__(self, source: str) -> None:
        self.source = source
        self.size = len(source)

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        i = 0
        while i < self.size:
            ch = self.source[i]

            if ch.isspace():
                i += 1
                continue

            if self.source.startswith("///", i):
                end = self._line_until(i + 3)
                tokens.append(Token(KIND_DOC, self.source[i:end], i, end))
                i = end
                continue

            if self.source.startswith("//", i):
                end = self._line_until(i + 2)
                tokens.append(Token(KIND_COMMENT, self.source[i:end], i, end))
                i = end
                continue

            if self.source.startswith("/**", i):
                end = self._block_until_comment(i + 3, "*/", "/**")
                tokens.append(Token(KIND_DOC, self.source[i:end], i, end))
                i = end
                continue

            if self.source.startswith("/*", i):
                end = self._block_until_comment(i + 2, "*/", "/*")
                tokens.append(Token(KIND_COMMENT, self.source[i:end], i, end))
                i = end
                continue

            if ch in {'"', "'"}:
                end = self._scan_string(i)
                tokens.append(Token(KIND_STRING, self.source[i:end], i, end))
                i = end
                continue

            if ch.isdigit():
                end = self._scan_number(i)
                tokens.append(Token(KIND_NUMBER, self.source[i:end], i, end))
                i = end
                continue

            if ch.isalpha() or ch == "_":
                end = self._scan_identifier(i)
                text = self.source[i:end]
                tokens.append(Token(self._identifier_kind(text), text, i, end))
                i = end
                continue

            if ch in "@#":
                end = self._scan_annotation(i, ch)
                kind = KIND_ANNOTATION if ch == "@" else KIND_ATTRIBUTE
                tokens.append(Token(kind, self.source[i:end], i, end))
                i = end
                continue

            matched = self._match_operator(i)
            if matched is not None:
                end, text = matched
                tokens.append(Token(KIND_OPERATOR, text, i, end))
                i = end
                continue

            if ch in PUNCTUATION:
                tokens.append(Token(KIND_PUNCTUATION, ch, i, i + 1))
                i += 1
                continue

            tokens.append(Token(KIND_UNKNOWN, ch, i, i + 1))
            i += 1

        return tokens

    @staticmethod
    def _identifier_kind(text: str) -> str:
        if text in KEYWORDS:
            return KIND_KEYWORD
        if text in TYPES:
            return KIND_TYPE
        if text in LITERALS:
            return KIND_LITERAL
        return KIND_IDENTIFIER

    def _line_until(self, i: int) -> int:
        while i < self.size and self.source[i] != "\n":
            i += 1
        return i

    def _block_until_comment(self, i: int, closing: str, opening: str) -> int:
        while True:
            if i >= self.size:
                return self.size
            if self.source.startswith(closing, i):
                return i + len(closing)
            i += 1

    def _scan_string(self, start: int) -> int:
        quote = self.source[start]
        i = start + 1
        while i < self.size:
            if self.source[i] == "\\":
                i += 2
                continue
            if self.source[i] == quote:
                return i + 1
            i += 1
        return self.size

    def _scan_number(self, start: int) -> int:
        i = start
        if self.source[i] == "0" and i + 1 < self.size:
            prefix = self.source[i + 1]
            if prefix in "xXbBcC":
                digits = {
                    "x": "[0-9A-Fa-f]",
                    "X": "[0-9A-Fa-f]",
                    "b": "[01]",
                    "B": "[01]",
                    "c": "[0-7]",
                    "C": "[0-7]",
                }[prefix]
                i += 2
                while i < self.size and re.fullmatch(digits, self.source[i]):
                    i += 1
                return i

        while i < self.size and self.source[i].isdigit():
            i += 1
        if (
            i + 1 < self.size
            and self.source[i] == "."
            and self.source[i + 1].isdigit()
        ):
            i += 1
            while i < self.size and self.source[i].isdigit():
                i += 1
        return i

    def _scan_identifier(self, start: int) -> int:
        i = start + 1
        while i < self.size and (self.source[i].isalnum() or self.source[i] == "_"):
            i += 1
        return i

    def _scan_annotation(self, start: int, prefix: str) -> int:
        i = start + 1
        if i < self.size and (self.source[i].isalpha() or self.source[i] == "_"):
            i = self._scan_identifier(i)
        else:
            return start + 1
        return i

    def _match_operator(self, i: int) -> tuple[int, str] | None:
        for operator in OPERATORS:
            if self.source.startswith(operator, i):
                return i + len(operator), operator
        return None


def render(token: Token, style: str) -> str:
    text = token.text
    if style == "plain":
        return text
    if token.kind == KIND_IDENTIFIER:
        return text
    if token.kind == KIND_UNKNOWN:
        return text

    if style == "markdown":
        if token.kind in {KIND_KEYWORD, KIND_DOC}:
            return f"**{text}**"
        if token.kind in {KIND_TYPE, KIND_COMMENT}:
            return f"*{text}*"
        if token.kind in {
            KIND_LITERAL,
            KIND_STRING,
            KIND_NUMBER,
            KIND_ANNOTATION,
            KIND_ATTRIBUTE,
            KIND_OPERATOR,
        }:
            return f"`{text}`"
        if token.kind == KIND_PUNCTUATION:
            return text

    if style == "tag":
        if token.kind == KIND_KEYWORD:
            return f"<k>{text}</k>"
        if token.kind == KIND_TYPE:
            return f"<t>{text}</t>"
        if token.kind == KIND_LITERAL:
            return f"<l>{text}</l>"
        if token.kind == KIND_STRING:
            return f"<s>{text}</s>"
        if token.kind == KIND_NUMBER:
            return f"<n>{text}</n>"
        if token.kind == KIND_COMMENT:
            return f"<c>{text}</c>"
        if token.kind == KIND_DOC:
            return f"<d>{text}</d>"
        if token.kind == KIND_ANNOTATION:
            return f"<a>{text}</a>"
        if token.kind == KIND_ATTRIBUTE:
            return f"<r>{text}</r>"
        if token.kind == KIND_OPERATOR:
            return f"<o>{text}</o>"
        if token.kind == KIND_PUNCTUATION:
            return f"<p>{text}</p>"
        return text

    if style == "prefix":
        labels = {
            KIND_KEYWORD: "k",
            KIND_TYPE: "t",
            KIND_LITERAL: "l",
            KIND_STRING: "s",
            KIND_NUMBER: "n",
            KIND_COMMENT: "c",
            KIND_DOC: "d",
            KIND_ANNOTATION: "a",
            KIND_ATTRIBUTE: "r",
            KIND_OPERATOR: "o",
            KIND_PUNCTUATION: "p",
        }
        label = labels.get(token.kind)
        return f"[{label}:{text}]" if label else text

    if style == "compact":
        if token.kind in {KIND_KEYWORD, KIND_TYPE}:
            return text.upper()
        if token.kind == KIND_PUNCTUATION:
            return text
        return f"[{text}]"

    if style == "ansi":
        codes = {
            KIND_KEYWORD: "\033[1;35m",
            KIND_TYPE: "\033[1;34m",
            KIND_LITERAL: "\033[33m",
            KIND_STRING: "\033[32m",
            KIND_NUMBER: "\033[33m",
            KIND_COMMENT: "\033[2;90m",
            KIND_DOC: "\033[2;96m",
            KIND_ANNOTATION: "\033[1;33m",
            KIND_ATTRIBUTE: "\033[1;33m",
            KIND_OPERATOR: "\033[1;37m",
            KIND_PUNCTUATION: "",
        }
        code = codes.get(token.kind, "")
        return f"{code}{text}\033[0m" if code else text

    raise ValueError(f"unknown style: {style}")


def highlight(source: str, style: str) -> str:
    tokenizer = Tokenizer(source)
    out: list[str] = []
    cursor = 0
    for token in tokenizer.tokenize():
        if token.start > cursor:
            out.append(source[cursor : token.start])
        out.append(render(token, style))
        cursor = token.end
    if cursor < len(source):
        out.append(source[cursor:])
    return "".join(out)


def read_input(args: argparse.Namespace) -> list[tuple[str, str]]:
    inputs: list[tuple[str, str]] = []
    if args.string is not None:
        inputs.append(("<string>", args.string))
    for path in args.files:
        if path == "-":
            inputs.append(("<stdin>", sys.stdin.read()))
            continue
        p = Path(path)
        if not p.is_file():
            raise FileNotFoundError(f"no such file: {path}")
        inputs.append((path, p.read_text(encoding="utf-8")))
    if not inputs:
        inputs.append(("<stdin>", sys.stdin.read()))
    return inputs


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Render Zith source with visible ASCII highlighting."
    )
    parser.add_argument("files", nargs="*", metavar="FILE", help="Zith source files; '-' reads stdin")
    parser.add_argument(
        "--string",
        dest="string",
        default=None,
        help="highlight this inline Zith source string",
    )
    parser.add_argument(
        "--style",
        choices=("markdown", "tag", "prefix", "compact", "ansi", "plain"),
        default="markdown",
        help="output style (default: markdown)",
    )
    parser.add_argument(
        "--emit-tokens",
        action="store_true",
        help="debug: print one token per line instead of highlighted source",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run internal checks and exit",
    )
    return parser


def run_self_test() -> None:
    sample = '''
    /// doc
    import "stdio.h"

    struct Point {
        x: i32,
        y: i32,
    }

    fn main(): i32 {
        let total: i32 = 0b1010 + 0x0F * 2.5;
        @println("total=", total);
        #volatile var flag: bool = false;
        if (not flag and total >= 10) -> .? {
            return total;
        }
    }
'''.strip()
    expected = {
        KIND_DOC: ["/// doc"],
        KIND_KEYWORD: ["import", "struct", "fn", "let", "var", "if", "return"],
        KIND_TYPE: ["i32", "bool"],
        KIND_STRING: ['"stdio.h"', '"total="'],
        KIND_NUMBER: ["0b1010", "0x0F", "2.5"],
        KIND_ANNOTATION: ["@println"],
        KIND_ATTRIBUTE: ["#volatile"],
        KIND_OPERATOR: ["=", "+", "*", ">=", "->", "?"],
        KIND_PUNCTUATION: ["{", "}", "(", ")", ":", ",", ";", "."],
        KIND_IDENTIFIER: ["flag", "total"],
    }
    tokens = Tokenizer(sample).tokenize()
    for kind, texts in expected.items():
        got = [token.text for token in tokens if token.kind == kind]
        for text in texts:
            if text not in got:
                raise AssertionError(f"expected {kind} token {text!r}, got {got}")
    for token in tokens:
        if token.text == sample[token.start : token.end]:
            continue
        raise AssertionError(f"token span mismatch: {token}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    if args.self_test:
        run_self_test()
        print("self-test: ok", file=sys.stderr)
        return 0

    try:
        sources = read_input(args)
    except (OSError, UnicodeDecodeError, FileNotFoundError) as exc:
        print(f"zith-ascii: {exc}", file=sys.stderr)
        return 2

    show_name = len(sources) > 1
    for name, source in sources:
        if show_name:
            print(f"# {name}")
        if args.emit_tokens:
            for token in Tokenizer(source).tokenize():
                escaped = token.text.replace("\n", "\\n")
                print(f"{token.kind:5}  {escaped}")
        else:
            print(highlight(source, args.style), end="")
        if source and not source.endswith("\n"):
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
