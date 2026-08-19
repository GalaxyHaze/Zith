#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
import subprocess
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import assert_contains, assert_not_contains, run_generator


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

RULES_WITH_DUPLICATE = """\
[tokens]
identifier = true

[keywords]
If = ["if", "if"]
"""

RULES_WITH_NON_ASCII = """\
[tokens]
identifier = true

[keywords]
If = "cafés"
"""

RULES_WITH_MANY_KEYWORDS = """\
[tokens]
identifier = true

[keywords]
If = "if"
Else = "else"
For = "for"
Type = ["u8", "u16", "u32", "bool"]
"""


def run_lexer_generator(repo_root: Path, rules_text: str, out_dir: Path) -> None:
    rules_path = out_dir / "lexer.rules"
    rules_path.write_text(rules_text, encoding="utf-8")
    result = run_generator(repo_root, rules_path, out_dir)
    if result.returncode != 0:
        raise AssertionError(result.stderr)


def run_lexer_generator_expect_failure(
    repo_root: Path, rules_text: str, out_dir: Path, needle: str,
) -> None:
    rules_path = out_dir / "lexer.rules"
    rules_path.write_text(rules_text, encoding="utf-8")
    result = run_generator(repo_root, rules_path, out_dir)
    if result.returncode == 0:
        raise AssertionError(f"expected generator failure mentioning {needle!r}")
    if needle not in result.stderr:
        raise AssertionError(
            f"expected failure to mention {needle!r}; got:\n{result.stderr}"
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
        run_lexer_generator(repo_root, RULES_WITH_HOOKS, hooks_out)

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
        assert_not_contains(
            lexer_cpp,
            "add_lexeme(",
            "no arena-copy lexeme pool",
        )
        lexer_hpp = (hooks_out / "lexer.hpp").read_text(encoding="utf-8")
        assert_not_contains(lexer_hpp, "lexemeId", "Token has no lexeme id")
        assert_contains(
            lexer_hpp,
            "[[nodiscard]] TokenStream run(",
            "TokenStream run declaration",
        )
        assert_not_contains(lexer_hpp, "lexemesArena", "run has no lexeme arena")
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
        assert_not_contains(lexer_hpp, "FormattedToken", "no formatted token type")
        assert_not_contains(lexer_hpp, "formatToken", "no formatToken")
        assert_not_contains(lexer_hpp, "printToken", "no printToken")
        assert_contains(lexer_cpp, "TokenStream Lexer::run(", "TokenStream run definition")
        assert_not_contains(lexer_cpp, "lexemesArena", "run has no lexeme arena")
        assert_contains(
            lexer_cpp,
            "TokenStream tokenize(",
            "TokenStream tokenize definition",
        )
        assert_not_contains(lexer_cpp, "strings.intern(", "no lexeme interning")
        assert_not_contains(lexer_cpp, "emit_precomputed", "no precomputed lexeme IDs")
        assert_not_contains(lexer_cpp, "InternedId", "no interned IDs")
        if "->" in lexer_cpp:
            raise AssertionError("implicit arrow leaked into generated lexer.cpp")

        compile_actions_smoke(repo_root, compiler, hooks_out)

        arrow_out = tmpdir / "arrow"
        arrow_out.mkdir()
        run_lexer_generator(repo_root, RULES_WITH_ARROW, arrow_out)
        arrow_cpp = (arrow_out / "lexer.cpp").read_text(encoding="utf-8")
        assert_contains(arrow_cpp, "text[offset + 1] == '>'", "explicit arrow compound")

        phf_out = tmpdir / "phf"
        phf_out.mkdir()
        run_lexer_generator(repo_root, RULES_WITH_MANY_KEYWORDS, phf_out)
        kw_hpp = (phf_out / "keyword-table.hpp").read_text(encoding="utf-8")
        assert_contains(kw_hpp, "namespace detail", "PHF helper namespace present")
        assert_contains(kw_hpp, "constexpr uint64_t hash64", "hash64 present")
        assert_contains(kw_hpp, "bucket_seed", "bucket seeds table present")
        assert_contains(kw_hpp, "hash_table", "PHF slot table present")
        assert_contains(kw_hpp, "keyword_meta", "compact keyword metadata present")
        assert_contains(
            kw_hpp,
            'static_assert(allKeywordsPlaced(), "not all keywords placed in perfect hash")',
            "PHF placement static assertion",
        )
        assert_not_contains(kw_hpp, "std::memcpy", "no std::memcpy in lookup")
        assert_not_contains(kw_hpp, "std::pair", "no spelling duplication in metadata")
        assert_not_contains(kw_hpp, "keyword_spellings", "no linear spelling table")
        subprocess.run(
            [
                compiler,
                "-std=c++23",
                "-fsyntax-only",
                "-I",
                str(phf_out),
                "-I",
                str(repo_root / "src/frontend/lexer"),
                "-I",
                str(repo_root / "src"),
                str(phf_out / "keyword-table.hpp"),
            ],
            check=True,
            cwd=repo_root,
        )

        dup_out = tmpdir / "dup"
        dup_out.mkdir()
        run_lexer_generator_expect_failure(
            repo_root, RULES_WITH_DUPLICATE, dup_out, "keyword duplicada"
        )

        nonascii_out = tmpdir / "nonascii"
        nonascii_out.mkdir()
        run_lexer_generator_expect_failure(
            repo_root, RULES_WITH_NON_ASCII, nonascii_out, "nao-ASCII"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
