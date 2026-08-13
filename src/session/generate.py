#!/usr/bin/env python3
"""Generate session.hpp, session.cpp, dispatch.hpp and .gitignore from session.rules."""

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
    parse_typed_member,
    validate_cpp_type,
    write_generated as write_generated_files,
)

def parse_state_member(raw: str, line_no: int) -> tuple[str, str, str]:
    return parse_typed_member(raw, line_no, "state", requires_default=True)


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
    logical = join_logical_lines(text)

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


def dispatch_result_lines(rules: SessionRules) -> list[str]:
    lines = [
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
        lines.append("")
    return lines


def stage_result_slot_lines(rules: SessionRules) -> list[str]:
    lines = [
        "template <Stage S>",
        "struct StageResultSlot;",
        "",
    ]
    for stage in rules.stages:
        lines.append(f"template <> struct StageResultSlot<Stage::{stage}> {{")
        lines.append(
            f"    common::memory::Optional<common::memory::Result<"
            f"typename dispatch_result<Stage::{stage}>::type>> value;"
        )
        lines.append("};")
        lines.append("")
    return lines


def make_session_header(rules: SessionRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/memory/arena.hpp"',
        '#include "common/diagnostic/diagnostic.hpp"',
        '#include "common/memory/dyn-array.hpp"',
        '#include "common/memory/optional.hpp"',
        '#include "common/memory/result.hpp"',
        '#include "common/memory/span.hpp"',
        '#include "session/types.hpp"',
        "",
        "#include <tuple>",
        "#include <string>",
        "#include <utility>",
        "",
        "namespace toolkit::session {",
        "",
    ]
    first_stage = rules.stages[0]
    last_stage = rules.stages[-1]
    lines.extend(stage_enum_lines(rules))
    lines.extend(dispatch_result_lines(rules))
    lines.extend(stage_result_slot_lines(rules))
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
            "using Diagnostic = common::diagnostic::Diagnostic;",
            "",
            "struct StageError : common::memory::Error {",
            "    Stage stage;",
            "",
            "    explicit StageError(Stage stage);",
            "};",
            "",
            "using StageResult = common::memory::Result<Stage, StageError>;",
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
            "    common::memory::DynArray<Diagnostic> &diags() noexcept { return diagsStorage_; }",
            "    const common::memory::DynArray<Diagnostic> &diags() const noexcept { return diagsStorage_; }",
            "",
            "    StageResult run();",
            "    StageResult runTo(Stage target);",
            "    StageResult resume();",
            "",
            "    template <Stage S>",
            "    [[nodiscard]] bool hasStageResult() const noexcept {",
            "        return std::get<StageResultSlot<S>>(stageResults_).value.isValid();",
            "    }",
            "",
            "    template <Stage S>",
            "    [[nodiscard]] common::memory::Result<typename dispatch_result<S>::type> &"
            "stageResult() noexcept {",
            "        return std::get<StageResultSlot<S>>(stageResults_).value.value();",
            "    }",
            "",
            "    template <Stage S>",
            "    [[nodiscard]] const common::memory::Result<"
            "typename dispatch_result<S>::type> &stageResult() const noexcept {",
            "        return std::get<StageResultSlot<S>>(stageResults_).value.value();",
            "    }",
            "",
            "    template <Stage S>",
            "    void storeStageResult(",
            "        common::memory::Result<typename dispatch_result<S>::type> value) {",
            "        std::get<StageResultSlot<S>>(stageResults_).value = std::move(value);",
            "    }",
            "",
            "private:",
            "    Context *context_ = nullptr;",
            "    common::memory::Arena storageArena_;",
            "    common::memory::DynArray<Diagnostic> diagsStorage_;",
            "    std::tuple<"
            + ", ".join(f"StageResultSlot<Stage::{stage}>" for stage in rules.stages)
            + "> stageResults_;",
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
            "bool storeStage(Stage stage, CompilationSession &session);",
            "",
            "} // namespace toolkit::session",
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
        "namespace toolkit::session {",
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
        "        if (!storeStage(plan.current, *this))",
        "            return StageError{plan.current};",
        "",
        "        if (static_cast<int>(plan.current) == static_cast<int>(plan.target))",
        "            return StageResult{plan.current};",
        "        if (!plan.advance())",
        "            return StageResult{plan.current};",
        "    }",
        "    return StageResult{plan.current};",
        "}",
        "StageError::StageError(Stage stage_) : stage(stage_), common::memory::Error{stageLabel(stage_)} {}",
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
            "bool storeStage(Stage stage, CompilationSession &session) {",
            "    switch (stage) {",
        ]
    )
    for stage in rules.stages:
        lines.append(f"    case Stage::{stage}: {{")
        lines.append(
            f"        auto result = dispatch<Stage::{stage}>(session);"
        )
        lines.append("        if (!result)")
        lines.append("            return false;")
        if rules.stage_types()[stage] == "void":
            lines.append(
                f"        session.storeStageResult<Stage::{stage}>({{}});"
            )
        else:
            lines.append(
                f"        session.storeStageResult<Stage::{stage}>(std::move(result));"
            )
        lines.append("        return true;")
        lines.append("    }")
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
            "} // namespace toolkit::session",
            "",
        ]
    )
    return "\n".join(lines)


def make_dispatch_header(rules: SessionRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/memory/result.hpp"',
        '#include "session/session.hpp"',
        '#include "session/types.hpp"',
        "",
        "namespace toolkit::session {",
        "",
    ]
    for stage in rules.stages:
        lines.append(f"using {stage}Result = dispatch_result<Stage::{stage}>::type;")
        lines.append("")
    lines.append("template <Stage S>")
    lines.append("[[nodiscard]] common::memory::Result<typename dispatch_result<S>::type> dispatch(")
    lines.append("    CompilationSession &session);")
    lines.append("")
    lines.append("} // namespace toolkit::session")
    lines.append("")
    return "\n".join(lines)


def make_gitignore() -> str:
    return gitignore_lines(["session.hpp", "session.cpp", "dispatch.hpp"])


def write_generated(out_dir: Path, rules: SessionRules) -> list[Path]:
    return write_generated_files(
        out_dir,
        [
            ("session.hpp", make_session_header(rules)),
            ("session.cpp", make_session_source(rules)),
            ("dispatch.hpp", make_dispatch_header(rules)),
            (".gitignore", make_gitignore()),
        ],
    )


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

    written = write_generated(out_path, rules)
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
