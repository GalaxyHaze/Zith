#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import (
    assert_contains,
    assert_not_contains,
    compile_smoke,
    expect_error,
    run_generator,
    write_rules,
)


RULES_WITH_ACTIONS = """\
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End
onError: hooks::parser::recover()

[contexts]
TopLevel
Block

[context TopLevel]
Identifier push=Block action=hooks::parser::enterBlock()

[context Block parent=TopLevel]
Punctuation punc=";" pop=Block action=hooks::parser::semi()
"""

RULES_WITH_MULTI_KIND = """\
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End

[contexts]
TopLevel

[context TopLevel]
Fn, Identifier lexeme="fn" action=hooks::parser::fn()
"""

RULES_WITH_POP = """\
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End
onError: hooks::parser::recover()

[contexts]
TopLevel
Block

[context TopLevel]
Identifier push=Block action=hooks::parser::enterBlock()

[context Block parent=TopLevel]
Punctuation punc=";" pop=Block action=hooks::parser::semi()
"""

RULES_WITH_BUILDER = """\
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End

[contexts]
TopLevel
Expr

[context TopLevel builder=true]
Identifier push=Expr action=hooks::parser::enterExpr()

[context Expr parent=TopLevel builder=true]
Punctuation punc=";" action=hooks::parser::leaf()
"""


def compile_parser_smoke(repo_root: Path, compiler: str, include_root: Path) -> None:
    source = include_root / "smoke.cpp"
    source.write_text(
        """\
#include "frontend/parser/actions.hpp"

#include <cstdlib>
#include <string_view>

namespace hooks::parser {

using Parser = generated_parser::Parser<sample::ParseOutput>;
using Token = generated_lexer::Token;

Recovery recover(Parser &, const Token &) {
    return generated_parser::Recovery::Skip;
}

void enterBlock(Parser &parser, const Token &) {
    parser.setOutput(sample::ParseOutput{});
}

void semi(Parser &, const Token &) {}

} // namespace hooks::parser

int main() {
    common::memory::Arena arena;
    generated_lexer::Lexer lexer;
    common::memory::StringInterner strings(arena);
    generated_lexer::TokenStream tokens = lexer.run("x;", strings);
    generated_parser::Parser<sample::ParseOutput> parser(arena);
    const auto result = parser.parse(tokens, std::string_view("x;"));
    return result.isOk() ? 0 : 1;
}
""",
        encoding="utf-8",
    )
    compile_smoke(
        repo_root,
        compiler,
        include_root,
        source=source,
        include_dirs=[
            include_root,
            include_root / "frontend" / "parser",
            repo_root / "src/frontend/lexer",
            repo_root / "src",
            repo_root / "build/src",
        ],
    )


def generate_valid(repo_root: Path, tmpdir: Path) -> None:
    include_root = tmpdir / "include"
    out_dir = include_root / "frontend" / "parser"
    out_dir.mkdir(parents=True)
    types_path = out_dir / "types.hpp"
    types_path.write_text(
        """\
#pragma once

#include "common/memory/result.hpp"

#include <cstdint>
#include <string>

struct Span {
    uint32_t start = 0;
    uint32_t end = 0;
};

namespace sample {

struct ParserDiagnostic : common::memory::Error {
    Span span;
    std::string message;
};

struct ParseOutput {
    int count = 0;
    bool sawEnd = false;
};

} // namespace sample
""",
        encoding="utf-8",
    )
    parser_types = include_root / "parser" / "types.hpp"
    parser_types.parent.mkdir(parents=True, exist_ok=True)
    parser_types.write_text(types_path.read_text(encoding="utf-8"), encoding="utf-8")

    rules_path = write_rules(tmpdir, RULES_WITH_ACTIONS, generator="parser")
    result = run_generator(
        repo_root,
        rules_path,
        out_dir,
        generator=repo_root / "src/frontend/parser/generate.py",
        types_path=types_path,
    )
    if result.returncode != 0:
        raise AssertionError(result.stderr)

    parser_hpp = (out_dir / "parser.hpp").read_text(encoding="utf-8")
    actions_hpp = (out_dir / "actions.hpp").read_text(encoding="utf-8")
    assert_contains(parser_hpp, "enum class ParserState {", "state enum")
    assert_contains(parser_hpp, "TopLevel,", "state TopLevel")
    assert_contains(parser_hpp, "Block", "state Block from contexts")
    assert_not_contains(parser_hpp, "[state]", "state section removed")
    assert_not_contains(parser_hpp, "allow=", "allow removed")
    assert_contains(parser_hpp, "using OutputAlias = Output;", "output alias")
    assert_contains(
        parser_hpp,
        "using TokenStreamAlias = generated_lexer::TokenStream;",
        "token stream alias",
    )
    assert_contains(
        parser_hpp,
        "if (token.kind == TokenKind::End) {",
        "global end rule",
    )
    assert_not_contains(
        parser_hpp,
        "sawEnd",
        "end has no generated local action",
    )
    assert_contains(
        parser_hpp,
        "case generated_parser::Recovery::Skip:",
        "recovery skip",
    )
    assert_contains(
        actions_hpp,
        "void enterBlock(Parser &parser, const Token &token);",
        "action declaration",
    )
    assert_contains(
        parser_hpp,
        "Recovery recover(Parser &parser, const Token &token);",
        "recovery declaration with aliases",
    )
    assert_not_contains(
        parser_hpp,
        "void advance() noexcept",
        "TokenStream wrapper removed",
    )
    assert_not_contains(
        parser_hpp,
        "BuilderAlias builder_;",
        "builder disabled without builder contexts",
    )
    compile_parser_smoke(repo_root, sys.argv[2], include_root)

    two_sections = write_rules(
        tmpdir,
        """\
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End

[contexts]
TopLevel
Block
Inner

[context TopLevel]
Identifier push=Inner

[context Inner parent=Block,TopLevel]
Identifier

[context Inner parent=TopLevel,Block,Inner]
Punctuation
""",
        generator="parser",
    )
    two_result = run_generator(
        repo_root,
        two_sections,
        out_dir,
        generator=repo_root / "src/frontend/parser/generate.py",
        types_path=types_path,
    )
    if two_result.returncode != 0:
        raise AssertionError(two_result.stderr)
    two_hpp = (out_dir / "parser.hpp").read_text(encoding="utf-8")
    assert_contains(
        two_hpp,
        "stack_[stack_.size() - 3] == ParserState::Block",
        "second disjoint parent chain",
    )

    multi = write_rules(tmpdir, RULES_WITH_MULTI_KIND, generator="parser")
    multi_result = run_generator(
        repo_root,
        multi,
        out_dir,
        generator=repo_root / "src/frontend/parser/generate.py",
        types_path=types_path,
    )
    if multi_result.returncode != 0:
        raise AssertionError(multi_result.stderr)
    multi_hpp = (out_dir / "parser.hpp").read_text(encoding="utf-8")
    assert_contains(
        multi_hpp,
        "(token.kind == TokenKind::Fn || token.kind == TokenKind::Identifier)",
        "multi-kind rule",
    )
    assert_not_contains(multi_hpp, "topState() == ParserState::Module", "leak default")

    pop = write_rules(tmpdir, RULES_WITH_POP, generator="parser")
    pop_result = run_generator(
        repo_root,
        pop,
        out_dir,
        generator=repo_root / "src/frontend/parser/generate.py",
        types_path=types_path,
    )
    if pop_result.returncode != 0:
        raise AssertionError(pop_result.stderr)
    pop_hpp = (out_dir / "parser.hpp").read_text(encoding="utf-8")
    assert_contains(
        pop_hpp,
        "if (!popState(ParserState::Block))",
        "guarded pop helper",
    )
    compile_parser_smoke(repo_root, sys.argv[2], include_root)

    builder = write_rules(tmpdir, RULES_WITH_BUILDER, generator="parser")
    builder_result = run_generator(
        repo_root,
        builder,
        out_dir,
        generator=repo_root / "src/frontend/parser/generate.py",
        types_path=types_path,
    )
    if builder_result.returncode != 0:
        raise AssertionError(builder_result.stderr)
    builder_hpp = (out_dir / "parser.hpp").read_text(encoding="utf-8")
    assert_contains(
        builder_hpp,
        '#include "common/parser/builder.hpp"',
        "builder include",
    )
    assert_contains(
        builder_hpp,
        "using BuilderAlias = common::parser::OutputBuilder<sample::ParseOutput>;",
        "default builder alias",
    )
    assert_contains(
        builder_hpp,
        "BuilderAlias &builder() noexcept",
        "builder accessor",
    )
    assert_contains(
        builder_hpp,
        "BuilderAlias builder_;",
        "builder member",
    )
    assert_not_contains(
        builder_hpp,
        "void advance() noexcept",
        "TokenStream wrapper removed from builder variant",
    )


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="parser-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        generate_valid(repo_root, tmpdir)

        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context Missing]\nEnd",
            "regras para context nao declarado",
            "rules for undeclared context",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nNotTop\n\n[context NotTop]\nEnd",
            "TopLevel",
            "missing TopLevel",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nIdentifier push=Missing",
            "push para context nao declarado",
            "push undeclared context",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel parent=Missing]\nIdentifier",
            "parent para context nao declarado",
            "undeclared parent",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\nBlock\nA\n\n[context A parent=TopLevel]\nIdentifier\n\n[context A parent=TopLevel,Block]\nPunctuation",
            "cadeias de context sobrepostas",
            "overlapping parent chains",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: bad::Missing\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nEnd",
            "tipo inexistente em types.hpp",
            "missing diagnostic type",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[state]\nTopLevel\n\n[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nEnd",
            "secao desconhecida",
            "state section rejected",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nIdentifier unknown=1",
            "campo de regra desconhecido",
            "unknown rule filter",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nIdentifier pop=TopLevel push=Module",
            "push e pop",
            "push and pop in same rule",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\n\n[contexts]\nTopLevel\n\n[context TopLevel builder=true]\nIdentifier",
            "builder=true exige action",
            "builder requires action",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )
        expect_error(
            repo_root,
            tmpdir,
            "[parser]\ninput: std::string_view\noutput: sample::ParseOutput\ndiagnostic: sample::ParserDiagnostic\ntokenStream: generated_lexer::TokenStream\nbuilder: CustomBuilder<sample::ParseOutput>\n\n[contexts]\nTopLevel\n\n[context TopLevel]\nIdentifier",
            "builder em [parser] exige",
            "builder field requires context builder",
            generator=repo_root / "src/frontend/parser/generate.py",
            generator_kind="parser",
            out_dir=tmpdir / "include" / "frontend" / "parser",
            types_path=tmpdir / "include" / "frontend" / "parser" / "types.hpp",
            supports_types=True,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
