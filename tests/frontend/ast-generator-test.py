#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
import subprocess
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import (
    assert_contains,
    expect_error,
    run_generator,
    write_rules,
)


def compile_ast_smoke(repo_root: Path, compiler: str, include_root: Path) -> None:
    source = include_root / "smoke.cpp"
    source.write_text(SMOKE_CPP, encoding="utf-8")
    subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-fsyntax-only",
            "-I",
            str(include_root),
            "-I",
            str(repo_root / "src"),
            str(source),
        ],
        check=True,
        cwd=repo_root,
    )


VALID_RULES = """\
[Program]
body: common::memory::DynArray<Expr> = children

[Expr]
span: Span

[LiteralExpr]
span: Span
valueText: std::string_view

[CallExpr]
span: Span
callee: Expr = child
name: std::string_view
arguments: common::memory::DynArray<Expr> = children
"""

UNKNOWN_SECTION = """\
[not-a-node]
value: int = value
"""

NODE_FIELD_NO_TAG = """\
[Program]
body: common::memory::DynArray<Expr>

[Expr]
callee: Expr
"""

UNDECLARED_CHILD = """\
[Expr]
child: Missing = child
"""

BAD_VALUE_TYPE = """\
[Expr]
value: std::vector<int> = value
"""

OPTIONAL_LEAF_TAGS = """\
[Expr]
span: Span = value
name: std::string_view = string
"""

MISSING_TYPE = """\
[Expr]
span = value
"""

EMPTY_TAG = """\
[Expr]
span: Span =
"""

DUPLICATE_NODE = """\
[Expr]
value: int = value

[Expr]
value: int = value
"""

DUPLICATE_FIELD = """\
[Expr]
value: int = value
value: int = value
"""

SMOKE_CPP = """\
#include "frontend/ast/ast.hpp"
#include "frontend/ast/walk.hpp"

#include <cstdio>
#include <cstring>

int main() {
    common::memory::Arena arena;
    generated_ast::AstRoot ast(arena);
    generated_ast::Expr *callee =
        generated_ast::make<generated_ast::Expr>(ast, Span{0, 0});
    generated_ast::CallExpr *call = generated_ast::make<generated_ast::CallExpr>(
        ast, Span{0, 1}, callee, "f");
    generated_ast::Program *program = generated_ast::make<generated_ast::Program>(ast);
    if (call == nullptr || program == nullptr)
        return 1;
    program->body.push(static_cast<generated_ast::AstNode *>(call));
    ast.root = program;

    int counts[3] = {};
    generated_ast::walk(ast, [&](auto *, auto *parent) {
        ++counts[0];
        if (parent == nullptr)
            ++counts[1];
    });
    if (counts[0] != 2 || counts[1] != 1)
        return 2;

    FILE *out = tmpfile();
    if (out == nullptr)
        return 3;
    generated_ast::print(ast, out);
    std::rewind(out);
    char buffer[512];
    const size_t n = std::fread(buffer, 1, sizeof(buffer), out);
    std::fclose(out);
    if (std::strstr(buffer, "CallExpr") == nullptr ||
        std::strstr(buffer, "Program") == nullptr)
        return 4;
    return 0;
}
"""


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="ast-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        include_root = tmpdir / "include"
        out_dir = include_root / "frontend" / "ast"
        out_dir.mkdir(parents=True)

        rules_path = write_rules(tmpdir, VALID_RULES, "ast")
        valid = run_generator(repo_root, rules_path, out_dir)
        if valid.returncode != 0:
            raise AssertionError(valid.stderr)

        ast_hpp = (out_dir / "ast.hpp").read_text(encoding="utf-8")
        ast_cpp = (out_dir / "ast.cpp").read_text(encoding="utf-8")
        walk_hpp = (out_dir / "walk.hpp").read_text(encoding="utf-8")
        assert_contains(ast_hpp, "struct AstRoot", "AstRoot declaration")
        assert_contains(ast_hpp, "T *make(AstRoot &ast", "make helper")
        assert_contains(ast_hpp, "void print(const AstRoot &ast", "print helper")
        assert_contains(ast_cpp, "void free(AstRoot &ast)", "free definition")
        assert_contains(walk_hpp, "void walk(AstRoot &ast", "walk helper")

        compile_ast_smoke(repo_root, compiler, include_root)

        expect_error(
            repo_root,
            tmpdir,
            UNKNOWN_SECTION,
            "secao desconhecida",
            "unknown section",
        )
        expect_error(
            repo_root,
            tmpdir,
            NODE_FIELD_NO_TAG,
            "use child ou children",
            "declared node field without tag",
        )
        expect_error(
            repo_root,
            tmpdir,
            MISSING_TYPE,
            "campo sem tipo",
            "field without type",
        )
        expect_error(
            repo_root,
            tmpdir,
            EMPTY_TAG,
            "tag vazia",
            "field with empty tag",
        )
        optional_tags = run_generator(
            repo_root,
            write_rules(tmpdir, OPTIONAL_LEAF_TAGS, "ast"),
            out_dir,
        )
        if optional_tags.returncode != 0:
            raise AssertionError(optional_tags.stderr)
        expect_error(
            repo_root,
            tmpdir,
            UNDECLARED_CHILD,
            "child para tipo nao declarado",
            "undeclared child type",
        )
        expect_error(
            repo_root,
            tmpdir,
            BAD_VALUE_TYPE,
            "tipo de campo nao suportado",
            "unsupported value type",
        )
        expect_error(
            repo_root,
            tmpdir,
            DUPLICATE_NODE,
            "no repetido",
            "duplicate node",
        )
        expect_error(
            repo_root,
            tmpdir,
            DUPLICATE_FIELD,
            "campo repetido",
            "duplicate field",
        )

    return 0
if __name__ == "__main__":
    raise SystemExit(main())
