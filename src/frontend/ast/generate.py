#!/usr/bin/env python3
"""Generate ast.hpp, ast.cpp, walk.hpp and .gitignore from ast.rules."""

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
    cpp_string,
    gitignore_lines,
    join_logical_lines,
    strip_comment,
    write_generated as write_generated_files,
)


SUPPORTED_TAGS = {"child", "children", "string", "value"}
VALUE_TYPES = {
    "bool",
    "char",
    "int",
    "long",
    "float",
    "double",
    "Span",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "size_t",
}


@dataclass
class Field:
    name: str
    cpp_type: str
    tag: str
    line_no: int


@dataclass
class Node:
    name: str
    line_no: int
    fields: list[Field] = field(default_factory=list)

    @property
    def node_fields(self) -> list[Field]:
        return [field for field in self.fields if field.tag in {"child", "children"}]

    @property
    def leaf_fields(self) -> list[Field]:
        return [field for field in self.fields if field.tag in {"", "string", "value"}]


def parse_rules(text: str) -> list[Node]:
    nodes: list[Node] = []
    current: Node | None = None
    field_names: set[str] = set()

    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue

        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            name = header.group(1).strip()
            if not re.fullmatch(r"[A-Z][A-Za-z0-9_]*", name):
                raise RuleError(line_no, f"secao desconhecida: [{header.group(1).strip()}]")
            if any(node.name == name for node in nodes):
                raise RuleError(line_no, f"no repetido: {name!r}")
            current = Node(name=name, line_no=line_no)
            field_names = set()
            nodes.append(current)
            continue

        if current is None:
            raise RuleError(line_no, f"campo fora de uma secao de no: {line!r}")

        if ":" not in line:
            raise RuleError(line_no, f"campo sem tipo: {line!r}")
        name, rhs = (part.strip() for part in line.split(":", 1))
        if "=" in rhs:
            cpp_type, tag = (part.strip() for part in rhs.split("=", 1))
        else:
            cpp_type, tag = rhs, ""
        if not tag and "=" in rhs:
            raise RuleError(line_no, f"campo com tag vazia: {line!r}")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise RuleError(line_no, f"nome de campo invalido: {name!r}")
        if name in field_names:
            raise RuleError(line_no, f"campo repetido em [{current.name}]: {name!r}")
        if tag and tag not in SUPPORTED_TAGS:
            raise RuleError(line_no, f"tag de campo nao suportada: {tag!r}")
        if not cpp_type:
            raise RuleError(line_no, f"tipo de campo vazio: {name!r}")
        field_names.add(name)
        current.fields.append(Field(name=name, cpp_type=cpp_type, tag=tag, line_no=line_no))

    if not nodes:
        raise RuleError(1, "ast.rules tem de declarar pelo menos um no")
    return nodes


def declared_names(nodes: list[Node]) -> set[str]:
    return {node.name for node in nodes}


def array_element(cpp_type: str) -> str | None:
    match = re.fullmatch(r"common::memory::DynArray<([A-Za-z_][A-Za-z0-9_]*)>", cpp_type.strip())
    return match.group(1) if match else None


def validate_rules(nodes: list[Node]) -> None:
    names = declared_names(nodes)
    for node in nodes:
        for field in node.fields:
            if field.tag == "child":
                if field.cpp_type not in names:
                    raise RuleError(
                        field.line_no,
                        f"child para tipo nao declarado: {field.cpp_type!r}",
                    )
            elif field.tag == "children":
                element = array_element(field.cpp_type)
                if element is None or element not in names:
                    raise RuleError(
                        field.line_no,
                        "children tem de ser common::memory::DynArray<TipoDeclarado>, "
                        f"mas o tipo foi {field.cpp_type!r}",
                    )
            elif field.tag == "string":
                if field.cpp_type != "std::string_view":
                    raise RuleError(
                        field.line_no,
                        f"campo string tem de usar std::string_view: {field.cpp_type!r}",
                    )
            elif field.tag == "value":
                if field.cpp_type not in VALUE_TYPES:
                    raise RuleError(
                        field.line_no,
                        f"tipo de campo nao suportado para tag value: {field.cpp_type!r}",
                    )
            else:
                child = array_element(field.cpp_type)
                if field.cpp_type in names or (child is not None and child in names):
                    raise RuleError(
                        field.line_no,
                        f"campo sem tag nao pode referenciar declarado no: "
                        f"{field.cpp_type!r}; use child ou children",
                    )
                if (
                    field.cpp_type != "std::string_view"
                    and field.cpp_type not in VALUE_TYPES
                ):
                    raise RuleError(
                        field.line_no,
                        f"tipo de campo nao suportado para folha: {field.cpp_type!r}",
                    )


def root_name(nodes: list[Node]) -> str:
    return "Program" if any(node.name == "Program" for node in nodes) else nodes[0].name


def node_field_type(field: Field) -> str:
    if field.tag == "child":
        return "AstNode *"
    if field.tag == "children":
        return "common::memory::DynArray<AstNode *>"
    return field.cpp_type


def node_constructor_params(node: Node) -> list[str]:
    params = ["common::memory::Arena &arena"]
    for field in node.fields:
        if field.tag != "children":
            param_type = (
                "AstNode *"
                if field.tag == "child"
                else field.cpp_type
            )
            params.append(f"{param_type} {field.name}")
    return params


def node_constructor_init(node: Node) -> list[str]:
    inits = [f"AstNode(NodeKind::{node.name})"]
    for field in node.fields:
        if field.tag == "children":
            inits.append(f"{field.name}(arena)")
        else:
            inits.append(f"{field.name}({field.name})")
    return inits


def make_kinds_and_structs(nodes: list[Node]) -> list[str]:
    lines: list[str] = []
    lines.append("struct AstRoot;")
    lines.append("")
    for node in nodes:
        lines.append(f"struct {node.name};")
    lines.append("")
    lines.append("enum class NodeKind {")
    for index, node in enumerate(nodes):
        end = "," if index + 1 < len(nodes) else ""
        lines.append(f"    {node.name}{end}")
    lines.append("};")
    lines.append("")
    lines.append("[[nodiscard]] constexpr const char *nodeKindName(NodeKind kind) noexcept {")
    lines.append("    switch (kind) {")
    for node in nodes:
        lines.append(f"    case NodeKind::{node.name}: return {cpp_string(node.name)};")
    lines.append("    }")
    lines.append('    return "???";')
    lines.append("}")
    lines.append("")
    lines.append("struct AstNode {")
    lines.append("    NodeKind kind;")
    lines.append("    explicit AstNode(NodeKind value) : kind(value) {}")
    lines.append("    [[nodiscard]] NodeKind nodeKind() const noexcept { return kind; }")
    lines.append("};")
    lines.append("")

    for node in nodes:
        params = node_constructor_params(node)
        inits = node_constructor_init(node)
        lines.append(f"struct {node.name} : AstNode {{")
        for field in node.fields:
            lines.append(f"    {node_field_type(field)} {field.name};")
        if len(inits) == 1:
            lines.append(f"    explicit {node.name}({', '.join(params)})")
            lines.append(f"        : {inits[0]} {{}}")
        else:
            lines.append(f"    explicit {node.name}({', '.join(params)})")
            lines.append("        : " + ",\n          ".join(inits) + " {}")
        lines.append("};")
        lines.append("")
    return lines


def make_ast_header(nodes: list[Node]) -> str:
    root = root_name(nodes)
    lines = [
        "#pragma once",
        "",
        '#include "common/ast/clone.hpp"',
        '#include "common/memory/arena.hpp"',
        '#include "common/memory/dyn-array.hpp"',
        '#include "frontend/ast/types.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdio>",
        "#include <string_view>",
        "#include <type_traits>",
        "#include <utility>",
        "",
        "namespace generated_ast {",
        "",
    ]
    lines.extend(make_kinds_and_structs(nodes))
    lines.extend(make_foreach_header(nodes))
    lines.extend(
        [
            "struct AstRoot {",
            "    common::memory::Arena *arena = nullptr;",
            f"    {root} *root = nullptr;",
            "    common::memory::DynArray<const void *> nodes;",
            "",
            "    explicit AstRoot(common::memory::Arena &a);",
            "    ~AstRoot();",
            "    AstRoot(AstRoot &&) noexcept;",
            "    AstRoot &operator=(AstRoot &&) noexcept;",
            "    AstRoot(const AstRoot &) = delete;",
            "    AstRoot &operator=(const AstRoot &) = delete;",
            "",
            "    [[nodiscard]] auto nodeCount() const noexcept -> size_t;",
            "    [[nodiscard]] bool contains(const void *node) const noexcept;",
            "};",
            "",
            "template <typename T, typename... Args>",
            "[[nodiscard]] T *alloc(AstRoot &ast, Args &&...args);",
            "",
            "template <typename T, typename... Args>",
            "[[nodiscard]] T *make(AstRoot &ast, Args &&...args) {",
            "    return alloc<T>(ast, std::forward<Args>(args)...);",
            "}",
            "",
            "void free(AstRoot &ast) noexcept;",
            "void print(const AstRoot &ast, FILE *out = stdout);",
            "",
            "template <typename T, typename... Args>",
            "[[nodiscard]] T *alloc(AstRoot &ast, Args &&...args) {",
            "    static_assert(std::is_base_of_v<AstNode, T>);",
            "    static_assert(std::is_constructible_v<T, common::memory::Arena &, Args...>);",
            "    if (ast.arena == nullptr)",
            "        return nullptr;",
            "    T *node = ast.arena->make<T>(*ast.arena, std::forward<Args>(args)...);",
            "    if (node != nullptr)",
            "        ast.nodes.push(node);",
            "    return node;",
            "}",
            "",
            "} // namespace generated_ast",
            "",
        ]
    )
    lines.append("namespace generated_ast {")
    lines.append("")
    lines.append("// cloneInto must be visible after AstRoot and alloc/make are defined.")
    lines.append("")
    lines.extend(make_clone_into_header(nodes))
    lines.append("} // namespace generated_ast")
    lines.append("")
    return "\n".join(lines)


def make_clone_into_header(nodes: list[Node]) -> list[str]:
    lines = []
    lines.extend(
        [
            "template <typename AstRoot>",
            "AstNode *cloneInto(AstRoot &ast, AstNode *source);",
            "",
        ]
    )
    for node in nodes:
        lines.append("template <typename AstRoot>")
        lines.append(
            f"{node.name} *cloneInto(AstRoot &ast, {node.name} *source);"
        )
    lines.append("")
    for node in nodes:
        lines.append("template <typename AstRoot>")
        lines.append(
            f"{node.name} *cloneInto(AstRoot &ast, {node.name} *source) {{"
        )
        lines.append("    if (source == nullptr)")
        lines.append("        return nullptr;")
        for field in node.fields:
            if field.tag == "child":
                lines.append(
                    f"    auto *cloned_{field.name} = "
                    f"common::ast::cloneNode(ast, source->{field.name});"
                )
        alloc_args = []
        for field in node.fields:
            if field.tag == "children":
                continue
            name = field.name
            if field.tag == "child":
                alloc_args.append(f"cloned_{name}")
            else:
                alloc_args.append(f"source->{name}")
        invocation = f"make<{node.name}>(ast"
        if alloc_args:
            invocation += ", " + ", ".join(alloc_args)
        invocation += ")"
        lines.append(f"    auto *clone = {invocation};")
        for field in node.fields:
            if field.tag == "children":
                lines.append(
                    f"    for (AstNode *child : source->{field.name})"
                )
                lines.append(
                    f"        clone->{field.name}.push("
                    f"common::ast::cloneNode(ast, child));"
                )
        lines.append("    return clone;")
        lines.append("}")
        lines.append("")
    lines.extend(
        [
            "template <typename AstRoot>",
            "AstNode *cloneInto(AstRoot &ast, AstNode *source) {",
            "    if (source == nullptr)",
            "        return nullptr;",
            "    switch (source->kind) {",
        ]
    )
    for node in nodes:
        lines.append(f"    case NodeKind::{node.name}:")
        lines.append(
            f"        return cloneInto(ast, "
            f"static_cast<{node.name} *>(source));"
        )
    lines.extend(
        [
            "    }",
            "    return nullptr;",
            "}",
            "",
        ]
    )
    return lines


def make_foreach_header(nodes: list[Node]) -> list[str]:
    lines = [
        "",
        "template <typename Fn>",
        "void for_each_child(AstNode *node, Fn &&fn);",
        "",
    ]
    for node in nodes:
        if node.node_fields:
            lines.append("template <typename Fn>")
            lines.append(
                f"void for_each_child({node.name} *node, Fn &&fn);"
            )
    lines.append("")
    for node in nodes:
        if not node.node_fields:
            continue
        lines.append("template <typename Fn>")
        lines.append(
            f"void for_each_child({node.name} *node, Fn &&fn) {{"
        )
        lines.append("    if (node == nullptr)")
        lines.append("        return;")
        for field in node.node_fields:
            if field.tag == "child":
                lines.append(f"    fn(node->{field.name});")
            else:
                lines.append(f"    for (AstNode *&candidate : node->{field.name})")
                lines.append("        fn(candidate);")
        lines.append("}")
        lines.append("")
    lines.append(
        "template <typename Fn>"
    )
    lines.append(
        "void for_each_child(AstNode *node, Fn &&fn) {"
    )
    lines.append("    if (node == nullptr)")
    lines.append("        return;")
    lines.append("    switch (node->kind) {")
    for node in nodes:
        lines.append(f"    case NodeKind::{node.name}:")
        if node.node_fields:
            lines.append(
                f"        for_each_child(static_cast<{node.name} *>(node), "
                "std::forward<Fn>(fn));"
            )
        lines.append("        break;")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    return lines


def make_ast_source(nodes: list[Node]) -> str:
    lines = [
        '#include "frontend/ast/ast.hpp"',
        "",
        "#include <cinttypes>",
        "#include <cstdio>",
        "",
        "namespace generated_ast {",
        "namespace {",
        "",
        "void print_indent(int depth, FILE *out) {",
        "    for (int i = 0; i < depth; ++i)",
        "        std::fputc(' ', out);",
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, Span value) {",
        '    std::fprintf(out, " %s=[%u,%u]", name, value.start, value.end);',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, std::string_view value) {",
        '    std::fprintf(out, " %s=%.*s", name, static_cast<int>(value.size()), value.data());',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, bool value) {",
        '    std::fprintf(out, " %s=%s", name, value ? "true" : "false");',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, char value) {",
        '    std::fprintf(out, " %s=%c", name, value);',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, int value) {",
        '    std::fprintf(out, " %s=%d", name, value);',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, long value) {",
        '    std::fprintf(out, " %s=%ld", name, value);',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, float value) {",
        '    std::fprintf(out, " %s=%g", name, static_cast<double>(value));',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, double value) {",
        '    std::fprintf(out, " %s=%g", name, value);',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, uint8_t value) {",
        '    std::fprintf(out, " %s=%u", name, static_cast<unsigned>(value));',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, uint16_t value) {",
        '    std::fprintf(out, " %s=%u", name, static_cast<unsigned>(value));',
        "}",
        "",
        "void print_leaf(FILE *out, const char *name, uint64_t value) {",
        '    std::fprintf(out, " %s=%" PRIu64, name, value);',
        "}",
        "",
        "void print_node(const AstRoot &ast, AstNode *node, int depth, FILE *out);",
        "",
    ]

    for node in nodes:
        lines.append(f"void print_node_typed(const AstRoot &ast, {node.name} *node, int depth, FILE *out) {{")
        lines.append("    if (node == nullptr) return;")
        lines.append("    print_indent(depth, out);")
        lines.append(f'    std::fprintf(out, "{node.name}");')
        for field in node.leaf_fields:
            lines.append(f"    print_leaf(out, {cpp_string(field.name)}, node->{field.name});")
        lines.append("    std::fputc('\\n', out);")
        for field in node.node_fields:
            if field.tag == "child":
                lines.append(f"    print_node(ast, node->{field.name}, depth + 2, out);")
            else:
                lines.append(f"    for (AstNode *child : node->{field.name})")
                lines.append("        print_node(ast, child, depth + 2, out);")
        lines.append("}")
        lines.append("")

    lines.append("void print_node(const AstRoot &ast, AstNode *node, int depth, FILE *out) {")
    lines.append("    if (node == nullptr) return;")
    lines.append("    switch (node->kind) {")
    for node in nodes:
        lines.append(
            f"    case NodeKind::{node.name}: print_node_typed(ast, "
            f"static_cast<{node.name} *>(node), depth, out); break;"
        )
    lines.append("    }")
    lines.append("}")
    lines.append("")

    lines.extend(
        [
            "} // anonymous namespace",
            "",
            "AstRoot::AstRoot(common::memory::Arena &a)",
            "    : arena(&a), root(nullptr), nodes(a) {}",
            "",
            "AstRoot::~AstRoot() { free(*this); }",
            "",
            "AstRoot::AstRoot(AstRoot &&) noexcept = default;",
            "AstRoot &AstRoot::operator=(AstRoot &&) noexcept = default;",
            "",
            "auto AstRoot::nodeCount() const noexcept -> size_t {",
            "    return nodes.size();",
            "}",
            "",
            "bool AstRoot::contains(const void *node) const noexcept {",
            "    for (const void *candidate : nodes)",
            "        if (candidate == node)",
            "            return true;",
            "    return false;",
            "}",
            "",
            "void free(AstRoot &ast) noexcept {",
            "    if (ast.arena != nullptr)",
            "        ast.arena->reset();",
            "    ast.nodes.clear();",
            "    ast.root = nullptr;",
            "}",
            "",
            "void print(const AstRoot &ast, FILE *out) {",
            "    print_node(ast, static_cast<AstNode *>(ast.root), 0, out);",
            "}",
            "",
            "} // namespace generated_ast",
            "",
        ]
    )
    return "\n".join(lines)


def make_walk_header(nodes: list[Node]) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "frontend/ast/ast.hpp"',
        "",
        "#include <type_traits>",
        "#include <utility>",
        "",
        "namespace generated_ast {",
        "",
        "template <typename Parent, typename Fn>",
        "void walkNode(AstRoot &ast, AstNode *node, Parent *parent, Fn &&fn);",
        "",
        "template <typename Fn>",
        "void walk(AstRoot &ast, Fn &&fn) {",
        "    if (ast.root == nullptr)",
        "        return;",
        "    using RootType = std::remove_pointer_t<decltype(ast.root)>;",
        "    walkNode(ast, static_cast<AstNode *>(ast.root),",
        "             static_cast<RootType *>(nullptr), std::forward<Fn>(fn));",
        "}",
        "",
    ]

    for node in nodes:
        lines.append("template <typename Parent, typename Fn>")
        lines.append(
            f"void walkNodeFields(AstRoot &ast, {node.name} *node, Parent *parent, Fn &&fn);"
        )
    lines.append("")

    lines.append("template <typename Parent, typename Fn>")
    lines.append("void walkNode(AstRoot &ast, AstNode *node, Parent *parent, Fn &&fn) {")
    lines.append("    if (node == nullptr) return;")
    lines.append("    fn(node, parent);")
    lines.append("    switch (node->kind) {")
    for node in nodes:
        lines.append(f"    case NodeKind::{node.name}:")
        if node.node_fields:
            lines.append(
                f"        walkNodeFields(ast, static_cast<{node.name} *>(node), parent, "
                f"std::forward<Fn>(fn));"
            )
        lines.append("        break;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    for node in nodes:
        if not node.node_fields:
            continue
        lines.append("template <typename Parent, typename Fn>")
        lines.append(
            f"void walkNodeFields(AstRoot &ast, {node.name} *node, Parent *parent, Fn &&fn)"
        )
        lines.append(" {")
        lines.append("    (void)ast; (void)parent;")
        for field in node.node_fields:
            if field.tag == "child":
                lines.append(f"    if (node->{field.name} != nullptr)")
                lines.append(
                    f"        walkNode(ast, node->{field.name}, node, std::forward<Fn>(fn));"
                )
            else:
                lines.append(f"    for (AstNode *child : node->{field.name})")
                lines.append(
                    f"        walkNode(ast, child, node, std::forward<Fn>(fn));"
                )
        lines.append("}")
        lines.append("")

    lines.append("} // namespace generated_ast")
    lines.append("")
    return "\n".join(lines)


def generated_files(nodes: list[Node]) -> list[tuple[str, str]]:
    return [
        ("ast.hpp", make_ast_header(nodes)),
        ("ast.cpp", make_ast_source(nodes)),
        ("walk.hpp", make_walk_header(nodes)),
        (".gitignore", make_gitignore()),
    ]


def make_gitignore() -> str:
    return gitignore_lines(["ast.hpp", "ast.cpp", "walk.hpp"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/frontend/ast/ast.rules")
    parser.add_argument("--out", default="build/src/frontend/ast")
    args = parser.parse_args()

    rules_path = Path(args.rules)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2

    try:
        nodes = parse_rules(rules_path.read_text(encoding="utf-8"))
        validate_rules(nodes)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    written = write_generated_files(out_path, generated_files(nodes))
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
