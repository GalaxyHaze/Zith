#!/usr/bin/env python3
"""Generate session.hpp, session.cpp, dispatch.hpp and .gitignore from session.rules."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


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


CPP_TYPE_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_:<>,. *&\[\]]*")


def validate_cpp_type(cpp_type: str, line_no: int, label: str) -> None:
    if not CPP_TYPE_PATTERN.fullmatch(cpp_type):
        raise RuleError(line_no, f"tipo de {label} invalido: {cpp_type!r}")


def parse_state_member(raw: str, line_no: int) -> tuple[str, str, str]:
    if "=" not in raw:
        raise RuleError(line_no, f"state sem default (falta '='): {raw!r}")
    lhs, rhs = raw.split("=", 1)
    lhs = lhs.strip()
    rhs = rhs.strip()
    if lhs.endswith(":"):
        lhs = lhs[:-1].strip()
    if ":" not in lhs:
        raise RuleError(line_no, f"state sem tipo: {raw!r}")
    name, cpp_type = (part.strip() for part in lhs.split(":", 1))
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise RuleError(line_no, f"nome de state invalido: {name!r}")
    validate_cpp_type(cpp_type, line_no, "state")
    return name, cpp_type, rhs


@dataclass
class StateField:
    name: str
    cpp_type: str
    default: str
    line_no: int


@dataclass
class SessionRules:
    context_type: str
    stages: list[str] = field(default_factory=list)
    stage_outputs: dict[str, str] = field(default_factory=dict)
    state_fields: list[StateField] = field(default_factory=list)

    def stage_types(self) -> dict[str, str]:
        return self.stage_outputs


def parse_rules(text: str, path: Path) -> SessionRules:
    section = ""
    context_type: str | None = None
    stages: list[str] = []
    stage_types: dict[str, tuple[str, int]] = {}
    state_fields: list[StateField] = []
    state_names: set[str] = set()
    logical: list[tuple[int, str]] = []
    pending: str | None = None
    pending_line = 0

    def flush_pending() -> None:
        nonlocal pending
        if pending is not None:
            logical.append((pending_line, pending))
            pending = None

    def balanced(body: str) -> bool:
        return (
            body.count("[") == body.count("]")
            and body.count("{") == body.count("}")
            and body.count('"') % 2 == 0
            and body.count("'") % 2 == 0
        )

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
            if balanced(pending):
                flush_pending()
        else:
            pending += " " + line
            if balanced(pending):
                flush_pending()
    flush_pending()

    for line_no, line in logical:
        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            section = header.group(1).strip().lower()
            if section not in {"context", "stages", "state"}:
                raise RuleError(line_no, f"secao desconhecida: [{section}]")
            continue

        if section == "context":
            key, sep, value = line.partition(":")
            key = key.strip()
            value = value.strip()
            if not sep:
                raise RuleError(line_no, f"contexto invalido: {line!r}")
            if key != "type":
                raise RuleError(line_no, f"entrada invalida em [context]: {key!r}")
            if context_type is not None:
                raise RuleError(line_no, "[context] aceita exatamente um tipo")
            validate_cpp_type(value, line_no, "context")
            context_type = value
            continue

        if section == "stages":
            stage, sep, output_type = line.partition(":")
            stage = stage.strip()
            output_type = output_type.strip() or "void"
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", stage):
                raise RuleError(line_no, f"nome de fase invalido: {line!r}")
            if stage in stages:
                raise RuleError(line_no, f"fase repetida: {stage!r}")
            validate_cpp_type(output_type, line_no, "output")
            stages.append(stage)
            stage_types[stage] = (output_type, line_no)
            continue

        if section == "state":
            name, cpp_type, default = parse_state_member(line, line_no)
            if name in state_names:
                raise RuleError(line_no, f"state repetido: {name!r}")
            state_names.add(name)
            state_fields.append(StateField(name=name, cpp_type=cpp_type, default=default, line_no=line_no))
            continue

        raise RuleError(line_no, f"secao desconhecida: [{section}]")

    if context_type is None:
        raise RuleError(1, "[context] tem de declarar exatamente um tipo")
    if not stages:
        raise RuleError(1, "[stages] tem de ter pelo menos uma fase")

    rules = SessionRules(
        context_type=context_type,
        stages=stages,
        stage_outputs={stage: stage_types[stage][0] for stage in stages},
        state_fields=state_fields,
    )
    for stage in stages:
        if stage not in stage_types:
            raise RuleError(1, f"fase sem tipo de output: {stage!r}")
    return rules


def stage_enum_lines(rules: SessionRules) -> list[str]:
    lines = ["enum class Stage {"]
    for index, stage in enumerate(rules.stages):
        end = "," if index + 1 < len(rules.stages) else ""
        lines.append(f"    {stage}{end}")
    lines.append("};")
    return lines


def make_session_header(rules: SessionRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/arena.hpp"',
        '#include "common/dyn-array.hpp"',
        '#include "common/result.hpp"',
        '#include "common/span.hpp"',
        '#include "session/types.hpp"',
        "",
        "#include <string>",
        "",
        "namespace zith::session {",
        "",
    ]
    first_stage = rules.stages[0]
    last_stage = rules.stages[-1]
    lines.extend(stage_enum_lines(rules))
    lines.extend(
        [
            "",
            "struct PipelinePlan {",
            f"    Stage current = Stage::{first_stage};",
            f"    Stage target = Stage::{last_stage};",
            "",
            "    [[nodiscard]] bool shouldStop() const noexcept {",
            "        return static_cast<int>(current) >= static_cast<int>(target);",
            "    }",
            "",
            "    [[nodiscard]] bool advance() noexcept {",
            "        if (shouldStop())",
            "            return false;",
            "        current = static_cast<Stage>(static_cast<int>(current) + 1);",
            "        return true;",
            "    }",
            "};",
            "",
            "struct Diagnostic {",
            "    memory::Span span{};",
            "    std::string message;",
            "};",
            "",
            "struct StageError : memory::Error {",
            "    Stage stage;",
            "",
            "    explicit StageError(Stage stage);",
            "};",
            "",
            "using StageResult = memory::Result<Stage, StageError>;",
            "",
            "const char *stageLabel(Stage stage) noexcept;",
            "",
            "struct CompilationSession {",
            f"    using Context = {rules.context_type};",
            "",
            "    PipelinePlan plan;",
            "    explicit CompilationSession(Context &context);",
            "    ~CompilationSession();",
            "",
            "    CompilationSession(const CompilationSession &) = delete;",
            "    CompilationSession &operator=(const CompilationSession &) = delete;",
            "",
            "    [[nodiscard]] bool hasErrors() const noexcept;",
            "    [[nodiscard]] Context &context() noexcept { return *context_; }",
            "    [[nodiscard]] const Context &context() const noexcept { return *context_; }",
            "    memory::DynArray<Diagnostic> &diags() noexcept { return diagsStorage_; }",
            "    const memory::DynArray<Diagnostic> &diags() const noexcept { return diagsStorage_; }",
            "",
            "    StageResult run();",
            "    StageResult runTo(Stage target);",
            "    StageResult resume();",
            "",
            "private:",
            "    Context *context_ = nullptr;",
            "    memory::Arena storageArena_;",
            "    memory::DynArray<Diagnostic> diagsStorage_;",
            "    bool errors_{false};",
        ]
    )
    if rules.state_fields:
        lines.append("")
        lines.append("public:")
        for state_field in rules.state_fields:
            lines.append(f"    {state_field.cpp_type} {state_field.name};")
        lines.append("")
    lines.extend(
        [
            "};",
            "",
            "bool dispatch(Stage stage, CompilationSession &session);",
            "",
            "} // namespace zith::session",
            "",
        ]
    )
    return "\n".join(lines)


def make_session_source(rules: SessionRules) -> str:
    init_exprs = [
        "context_(&context)",
        "storageArena_()",
        "diagsStorage_(storageArena_)",
        *[f"{state.name}({state.default})" for state in rules.state_fields],
    ]
    init_lines = ["CompilationSession::CompilationSession(Context &context)"]
    for index, expr in enumerate(init_exprs):
        suffix = "," if index + 1 < len(init_exprs) else ""
        prefix = "    : " if index == 0 else "      "
        init_lines.append(f"{prefix}{expr}{suffix}")

    lines = [
        '#include "session.hpp"',
        '#include "session/dispatch.hpp"',
        "",
        "namespace zith::session {",
        "",
        *init_lines,
        "{",
        "}",
        "",
        "CompilationSession::~CompilationSession() = default;",
        "",
        "bool CompilationSession::hasErrors() const noexcept {",
        "    return errors_ || !diagsStorage_.empty();",
        "}",
        "",
        "StageResult CompilationSession::run() {",
        f"    return runTo(Stage::{rules.stages[-1]});",
        "}",
        "",
        "StageResult CompilationSession::runTo(Stage target) {",
        "    plan.target = target;",
        f"    plan.current = Stage::{rules.stages[0]};",
        "    return resume();",
        "}",
        "",
        "StageResult CompilationSession::resume() {",
        "    while (static_cast<int>(plan.current) <= static_cast<int>(plan.target)) {",
        "        if (hasErrors())",
        "            return StageError{plan.current};",
        "",
        "        if (!dispatch(plan.current, *this))",
        "            return StageError{plan.current};",
        "",
        "        if (static_cast<int>(plan.current) == static_cast<int>(plan.target))",
        "            return StageResult{plan.current};",
        "        if (!plan.advance())",
        "            return StageResult{plan.current};",
        "    }",
        "    return StageResult{plan.current};",
        "}",
        "StageError::StageError(Stage stage_) : stage(stage_), memory::Error{stageLabel(stage_)} {}",
        "",
        "bool dispatch(Stage stage, CompilationSession &session) {",
        "    switch (stage) {",
    ]
    for stage in rules.stages:
        lines.append(
            f"    case Stage::{stage}: return static_cast<bool>(dispatch<Stage::{stage}>(session));"
        )
    lines.extend(
        [
            "    default: return false;",
            "    }",
            "}",
            "",
            "const char *stageLabel(Stage stage) noexcept {",
            "    switch (stage) {",
        ]
    )
    for stage in rules.stages:
        lines.append(f'    case Stage::{stage}: return "{stage}";')
    lines.extend(
        [
            "    default: return \"?\";",
            "    }",
            "}",
            "",
            "} // namespace zith::session",
            "",
        ]
    )
    return "\n".join(lines)


def make_dispatch_header(rules: SessionRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/result.hpp"',
        '#include "session/session.hpp"',
        '#include "session/types.hpp"',
        "",
        "namespace zith::session {",
        "",
        "template <Stage S>",
        "struct dispatch_result {",
        "    using type = void;",
        "};",
        "",
    ]
    for stage in rules.stages:
        output_type = rules.stage_types()[stage]
        lines.append(f"template <> struct dispatch_result<Stage::{stage}> {{")
        lines.append(f"    using type = {output_type};")
        lines.append("};")
        lines.append(f"using {stage}Result = dispatch_result<Stage::{stage}>::type;")
        lines.append("")
    lines.append("template <Stage S>")
    lines.append("[[nodiscard]] memory::Result<typename dispatch_result<S>::type> dispatch(")
    lines.append("    CompilationSession &session);")
    lines.append("")
    lines.append("} // namespace zith::session")
    lines.append("")
    return "\n".join(lines)


def make_gitignore() -> str:
    return "session.hpp\nsession.cpp\ndispatch.hpp\n__pycache__/\n"


def write_generated(out_dir: Path, rules: SessionRules) -> list[Path]:
    outputs = [
        ("session.hpp", make_session_header(rules)),
        ("session.cpp", make_session_source(rules)),
        ("dispatch.hpp", make_dispatch_header(rules)),
        (".gitignore", make_gitignore()),
    ]
    written: list[Path] = []
    for name, content in outputs:
        target = out_dir / name
        target.write_text(content, encoding="utf-8")
        written.append(target)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/session/session.rules")
    parser.add_argument("--out", default="build/src/session", help="diretorio de saida")
    args = parser.parse_args()

    rules_path = Path(args.rules)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2

    try:
        rules = parse_rules(rules_path.read_text(encoding="utf-8"), rules_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    out_path.mkdir(parents=True, exist_ok=True)
    written = write_generated(out_path, rules)
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
