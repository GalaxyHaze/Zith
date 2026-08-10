#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


RULES_WITH_HOOKS = """\
[tokens]
identifier = true
skip = " \\n\\t\\r"
punc = ";"
operators = "+-"
compound = ["+="]

[keywords]
If = "if"

[token-type]
channel: int = 0

[actions]
onLex = hooks::lexer::onLex()
onToken = hooks::lexer::onToken()
offLex = hooks::lexer::offLex()
"""

RULES_WITH_ARROW = """\
[tokens]
identifier = true
skip = " "
operators = "-"
compound = ["->"]
"""


def assert_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle!r}")


def run_generator(repo_root: Path, rules_text: str, out_dir: Path) -> None:
    rules_path = out_dir / "lexer.rules"
    rules_path.write_text(rules_text, encoding="utf-8")
    subprocess.run(
        [
            sys.executable,
            str(repo_root / "src/frontend/lexer/generate.py"),
            str(rules_path),
            "--out",
            str(out_dir),
            "--types",
            str(repo_root / "src/frontend/lexer/types.hpp"),
        ],
        check=True,
        cwd=repo_root,
    )


def compile_actions_smoke(repo_root: Path, compiler: str, out_dir: Path) -> None:
    source = out_dir / "actions.cpp"
    source.write_text(
        """\
#include "actions.hpp"

namespace hooks::lexer {
void onLex(generated_lexer::Lexer &, std::string_view) {}
void onToken(generated_lexer::Lexer &, const generated_lexer::Token &, std::string_view) {}
void offLex(
    generated_lexer::Lexer &,
    std::string_view,
    const generated_lexer::TokenStream &
) {}
} // namespace hooks::lexer
""",
        encoding="utf-8",
    )
    subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-fsyntax-only",
            "-I",
            str(out_dir),
            "-I",
            str(repo_root / "src/frontend/lexer"),
            "-I",
            str(repo_root / "src"),
            str(source),
        ],
        check=True,
        cwd=repo_root,
    )


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="lexer-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        hooks_out = tmpdir / "hooks"
        hooks_out.mkdir()
        run_generator(repo_root, RULES_WITH_HOOKS, hooks_out)

        actions_hpp = (hooks_out / "actions.hpp").read_text(encoding="utf-8")
        lexer_cpp = (hooks_out / "lexer.cpp").read_text(encoding="utf-8")

        assert_contains(actions_hpp, '#include "lexer.hpp"', "self-contained include")
        assert_contains(
            actions_hpp,
            "namespace hooks::lexer { void onLex(generated_lexer::Lexer &lexer, std::string_view source); }",
            "onLex declaration",
        )
        assert_contains(
            actions_hpp,
            "namespace hooks::lexer { void onToken(generated_lexer::Lexer &lexer, const generated_lexer::Token &token, std::string_view lexeme); }",
            "onToken declaration",
        )
        assert_contains(
            actions_hpp,
            "namespace hooks::lexer { void offLex(generated_lexer::Lexer &lexer, std::string_view source, const generated_lexer::TokenStream &tokens); }",
            "offLex declaration",
        )
        assert_contains(
            lexer_cpp,
            'constexpr std::array<std::string_view, 1> compound_set = {',
            "compound set size",
        )
        lexer_hpp = (hooks_out / "lexer.hpp").read_text(encoding="utf-8")
        assert_contains(lexer_hpp, "InternedId lexemeId", "Token lexeme id")
        assert_contains(
            lexer_hpp,
            "[[nodiscard]] TokenStream run(",
            "TokenStream run declaration",
        )
        assert_contains(
            lexer_hpp,
            "memory::StringInterner &strings",
            "StringInterner run declaration",
        )
        assert_contains(
            lexer_hpp,
            "[[nodiscard]] TokenStream tokenize(",
            "TokenStream tokenize declaration",
        )
        assert_contains(
            lexer_hpp,
            "TokenStream(std::vector<Token>",
            "vector TokenStream constructor",
        )
        assert_contains(
            lexer_hpp,
            "std::span<const Token>",
            "span TokenStream constructor and slices",
        )
        assert_contains(
            lexer_hpp,
            "struct FormattedToken",
            "FormattedToken declaration",
        )
        assert_contains(
            lexer_hpp,
            "FormattedToken formatToken(",
            "formatToken declaration",
        )
        assert_contains(
            lexer_hpp,
            "void printToken(",
            "printToken declaration",
        )
        assert_contains(
            lexer_hpp,
            "FILE *out",
            "FILE print overloads",
        )
        assert_contains(
            lexer_cpp,
            "FormattedToken formatToken(",
            "formatToken definition",
        )
        assert_contains(
            lexer_cpp,
            "void printToken(",
            "printToken definition",
        )
        assert_contains(lexer_cpp, "TokenStream Lexer::run(", "TokenStream run definition")
        assert_contains(
            lexer_cpp,
            "memory::StringInterner &strings",
            "StringInterner run definition",
        )
        assert_contains(
            lexer_cpp,
            "TokenStream tokenize(",
            "TokenStream tokenize definition",
        )
        assert_contains(
            lexer_cpp,
            "strings.intern(",
            "lexeme interning definition",
        )
        if "->" in lexer_cpp:
            raise AssertionError("implicit arrow leaked into generated lexer.cpp")

        compile_actions_smoke(repo_root, compiler, hooks_out)

        arrow_out = tmpdir / "arrow"
        arrow_out.mkdir()
        run_generator(repo_root, RULES_WITH_ARROW, arrow_out)
        arrow_cpp = (arrow_out / "lexer.cpp").read_text(encoding="utf-8")
        assert_contains(arrow_cpp, '"->"', "explicit arrow compound")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
