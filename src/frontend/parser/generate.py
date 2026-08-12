#!/usr/bin/env python3
"""Generate parser.hpp, parser.cpp and actions.hpp from parser.rules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve()
while not (_REPO_ROOT / "tools").is_dir():
    _REPO_ROOT = _REPO_ROOT.parent
sys.path.insert(0, str(_REPO_ROOT))

from tools.rules_kit import (
    RuleError,
    cpp_char,
    cpp_string,
    gitignore_lines,
    join_logical_lines,
    parse_hook as parse_hook_name,
    parse_quoted,
    split_top_level,
    strip_comment,
    validate_cpp_type,
    validate_identifier,
    write_generated as write_generated_files,
)

@dataclass(frozen=True)
class Hook:
    qualified_name: str
    line_no: int
    return_type: str = "void"

    @property
    def namespace(self) -> str:
        parts = self.qualified_name.rsplit("::", 1)
        return parts[0] if len(parts) == 2 else ""

    @property
    def name(self) -> str:
        return self.qualified_name.rsplit("::", 1)[-1]

    def declaration(self, output_cpp: str) -> str:
        del output_cpp
        return f"{self.return_type} {self.name}(Parser &parser, const Token &token);"


@dataclass
class Rule:
    line_no: int
    context: str
    parents: list[str]
    kinds: list[str]
    lexeme: str | None = None
    punc: str | None = None
    push: str | None = None
    pop: str | None = None
    action: Hook | None = None


@dataclass
class StateField:
    name: str
    cpp_type: str
    line_no: int


@dataclass
class ParserRules:
    input: str
    output: str
    diagnostic: str
    token_stream: str
    contexts: list[str]
    parents: dict[str, list[list[str]]]
    rules: list[Rule]
    end: str | None = None
    on_error: Hook | None = None
    context_builders: dict[str, bool] | None = None
    builder: str | None = None
    state_members: dict[str, list[StateField]] | None = None

    @property
    def states(self) -> list[str]:
        return self.contexts

    @property
    def actions(self) -> list[Hook]:
        seen: set[str] = set()
        out: list[Hook] = []
        for rule in self.rules:
            if rule.action is None or rule.action.qualified_name in seen:
                continue
            seen.add(rule.action.qualified_name)
            out.append(rule.action)
        return out

    @property
    def top_state(self) -> str:
        return "TopLevel"

    @property
    def builder_enabled(self) -> bool:
        return bool(self.builder) or bool(self.context_builders and
                                          any(self.context_builders.values()))

    @property
    def builder_type(self) -> str:
        return self.builder or f"common::parser::OutputBuilder<{self.output}>"


def parse_hook(raw: str, line_no: int, return_type: str = "void") -> Hook:
    return Hook(
        qualified_name=parse_hook_name(raw, line_no),
        line_no=line_no,
        return_type=return_type,
    )


def parse_rule(raw: str, context: str, parents: list[str], line_no: int) -> Rule:
    fields: list[str] = []
    current: list[str] = []
    quote: str | None = None
    escaped = False
    index = 0
    while index < len(raw):
        ch = raw[index]
        if quote:
            current.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            index += 1
            continue
        if ch in {'"', "'"}:
            quote = ch
            current.append(ch)
            index += 1
            continue
        if ch.isspace():
            if current:
                fields.append("".join(current))
                current = []
            index += 1
            continue
        current.append(ch)
        index += 1
    if current:
        fields.append("".join(current))

    kinds: list[str] = []
    filters: list[str] = []
    for field_value in fields:
        if "=" in field_value:
            filters.append(field_value)
        else:
            kinds.extend(part for part in field_value.split(",") if part.strip())

    action: Hook | None = None
    lexeme: str | None = None
    punc: str | None = None
    push: str | None = None
    pop: str | None = None
    for filter_value in filters:
        key, sep, value = filter_value.partition("=")
        key = key.strip().lower()
        value = value.strip()
        if not sep:
            raise RuleError(line_no, f"campo de regra invalido: {filter_value!r}")
        if key == "action":
            action = parse_hook(value, line_no)
        elif key == "lexeme":
            lexeme = parse_quoted(value, line_no)
        elif key == "punc":
            punc = parse_quoted(value, line_no)
        elif key == "push":
            push = value.strip()
        elif key == "pop":
            pop = value.strip()
        else:
            raise RuleError(line_no, f"campo de regra desconhecido: {key!r}")

    if not kinds:
        raise RuleError(line_no, f"regra sem kinds em [{context}]")
    if lexeme is not None and not lexeme:
        raise RuleError(line_no, f"lexeme nao pode ser vazio: {lexeme!r}")
    if punc is not None and len(punc) != 1:
        raise RuleError(line_no, f"punc deve ser um caractere: {punc!r}")
    if push is not None and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", push):
        raise RuleError(line_no, f"push invalido: {push!r}")
    if pop is not None and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", pop):
        raise RuleError(line_no, f"pop invalido: {pop!r}")
    if push is not None and pop is not None:
        raise RuleError(line_no, "regra nao pode ter push e pop ao mesmo tempo")

    return Rule(
        line_no=line_no,
        context=context,
        parents=parents,
        kinds=[item for item in kinds],
        lexeme=lexeme,
        punc=punc,
        push=push,
        pop=pop,
        action=action,
    )


def parse_rules(text: str, path: Path) -> ParserRules:
    logical = join_logical_lines(text)

    contexts: list[str] = []
    context_set: set[str] = set()
    parser_fields: dict[str, tuple[str, int]] = {}
    context_parents: dict[str, list[list[str]]] = {}
    context_lines: dict[str, int] = {}
    context_builders: dict[str, bool] = {}
    rules: list[Rule] = []
    end: str | None = None
    on_error: Hook | None = None
    section = ""
    context = ""
    state_section: str | None = None
    state_fields_map: dict[str, list[StateField]] = {}

    for line_no, line in logical:
        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            raw_section = header.group(1).strip()
            lowered = raw_section.lower()
            if lowered == "parser":
                section = "parser"
                state_section = None
            elif lowered == "contexts":
                section = "contexts"
            elif lowered.startswith("context "):
                state_section = None
                raw_context = raw_section[len("context "):].strip()
                header_parts = raw_context.split()
                candidate = header_parts[0]
                validate_identifier(candidate, line_no, "context")
                context_builders.setdefault(candidate, False)
                context_set.add(candidate)
                if candidate not in context_lines:
                    context_lines[candidate] = line_no
                if candidate not in context_parents:
                    context_parents[candidate] = []
                parents: list[str] = []
                builder_seen = False
                for header_field in header_parts[1:]:
                    key, sep, value = header_field.partition("=")
                    key = key.strip().lower()
                    value = value.strip()
                    if not sep:
                        raise RuleError(
                            line_no,
                            f"campo de context invalido: {header_field!r}",
                        )
                    if key == "parent":
                        parents = [
                            item.strip()
                            for item in value.split(",")
                            if item.strip()
                        ]
                        if not parents:
                            raise RuleError(
                                line_no,
                                f"parent vazio em [context {raw_context}]",
                            )
                    elif key == "builder":
                        if builder_seen:
                            raise RuleError(
                                line_no,
                                f"builder repetido em [context {raw_context}]",
                            )
                        builder_seen = True
                        if value not in {"true", "false"}:
                            raise RuleError(
                                line_no,
                                f"builder deve ser true ou false: {value!r}",
                            )
                        context_builders[candidate] = value == "true"
                    else:
                        raise RuleError(
                            line_no,
                            f"campo de context invalido: {header_field!r}",
                        )
                context_parents[candidate].append(parents)
                section = "context"
                context = candidate
            elif lowered.startswith("state "):
                section = "state"
                raw_state = raw_section[len("state "):].strip()
                validate_identifier(raw_state, line_no, "state")
                state_section = raw_state
                state_fields_map.setdefault(raw_state, [])
            else:
                raise RuleError(line_no, f"secao desconhecida: [{raw_section}]")
            continue

        if section == "parser":
            if ":" not in line:
                raise RuleError(line_no, f"declaracao [parser] sem tipo: {line!r}")
            name, value = (part.strip() for part in line.split(":", 1))
            if name == "builder":
                validate_identifier(name, line_no, "parser type")
                validate_cpp_type(value, line_no, "parser type")
                if name in parser_fields:
                    raise RuleError(line_no, f"declaracao repetida: {name!r}")
                parser_fields[name] = (value, line_no)
                continue
            if name in {"end", "onError"}:
                if name == "end":
                    end_token = value.strip().rsplit("::", 1)[-1]
                    validate_identifier(end_token, line_no, "end")
                extra: Hook | None = None
                if name == "end":
                    extra = None
                elif name == "onError":
                    extra = parse_hook(value, line_no, return_type="Recovery")
                else:
                    raise RuleError(line_no, f"declaracao [parser] invalida: {name!r}")
                if name in parser_fields:
                    raise RuleError(line_no, f"declaracao repetida: {name!r}")
                parser_fields[name] = (value, line_no)
                if name == "end":
                    end = value
                elif name == "onError":
                    on_error = extra
                continue
            validate_cpp_type(value, line_no, "parser type")
            if name not in {"input", "output", "diagnostic", "tokenStream"}:
                raise RuleError(
                    line_no,
                    f"declaracao [parser] invalida: {name!r}",
                )
            if name in parser_fields:
                raise RuleError(line_no, f"declaracao repetida: {name!r}")
            parser_fields[name] = (value, line_no)
            continue

        if section == "contexts":
            value = line.strip()
            validate_identifier(value, line_no, "contexts")
            if value in contexts:
                raise RuleError(line_no, f"context repetido: {value!r}")
            contexts.append(value)
            continue

        if section == "state":
            if state_section is None:
                raise RuleError(line_no, f"member fora de [state]: {line!r}")
            if ":" not in line:
                raise RuleError(line_no, f"member sem tipo: {line!r}")
            name, cpp_type = (part.strip() for part in line.split(":", 1))
            validate_identifier(name, line_no, "state member")
            validate_cpp_type(cpp_type, line_no, "state member")
            fields = state_fields_map.setdefault(state_section, [])
            if any(field.name == name for field in fields):
                raise RuleError(
                    line_no,
                    f"member repetido em [{state_section}]: {name!r}",
                )
            fields.append(StateField(name=name, cpp_type=cpp_type, line_no=line_no))
            continue

        if section == "context":
            rules.append(parse_rule(line, context, parents, line_no))
            continue

        raise RuleError(line_no, f"secao desconhecida: [{section}]")

    if not contexts:
        raise RuleError(1, "[contexts] tem de declarar pelo menos TopLevel")
    if "TopLevel" not in contexts:
        raise RuleError(1, "[contexts] tem de declarar TopLevel")

    for context_name, parent_chains in context_parents.items():
        for chain in parent_chains:
            seen_parents: set[str] = set()
            for parent in chain:
                if parent not in contexts:
                    raise RuleError(
                        context_lines[context_name],
                        f"parent para context nao declarado: {parent!r}",
                    )
                if parent in context_set and parent != context_name and parent not in contexts:
                    raise RuleError(
                        context_lines[context_name],
                        f"parent nao pode ser filho de outro context: {parent!r}",
                    )
                if parent in seen_parents:
                    raise RuleError(
                        context_lines[context_name],
                        f"parent repetido na cadeia: {parent!r}",
                    )
                seen_parents.add(parent)

    for context_name, parent_chains in context_parents.items():
        for index, parents in enumerate(parent_chains):
            if not parents:
                continue
            for other_index, other_parents in enumerate(parent_chains):
                if index == other_index or not other_parents:
                    continue
                if is_prefix(parents, other_parents):
                    raise RuleError(
                        context_lines[context_name],
                        f"cadeias de context sobrepostas: {context_name}",
                    )

    if end is not None:
        validate_identifier(end.rsplit("::", 1)[-1], parser_fields["end"][1], "end")

    for rule in rules:
        if rule.context not in contexts:
            raise RuleError(
                rule.line_no,
                f"regras para context nao declarado: {rule.context!r}",
            )
        for kind in rule.kinds:
            validate_identifier(kind, rule.line_no, "kind")
        if rule.push is not None and rule.push not in contexts:
            raise RuleError(
                rule.line_no,
                f"push para context nao declarado: {rule.push!r}",
            )
        if rule.pop is not None and rule.pop not in contexts:
            raise RuleError(
                rule.line_no,
                f"pop para context nao declarado: {rule.pop!r}",
            )

    for state_name, fields in state_fields_map.items():
        if state_name not in contexts:
            raise RuleError(
                context_lines.get(state_name, 1),
                f"[state {state_name}] nao declarado nos contexts",
            )
        if not fields:
            raise RuleError(
                context_lines.get(state_name, 1),
                f"[state {state_name}] tem de declarar pelo menos um member",
            )

    builder_contexts = {
        name
        for name, enabled in context_builders.items()
        if enabled
    }
    for builder_context in sorted(builder_contexts):
        rules_for_context = [
            rule
            for rule in rules
            if rule.context == builder_context
        ]
        if not any(rule.action is not None for rule in rules_for_context):
            raise RuleError(
                context_lines[builder_context],
                f"builder=true exige action no context {builder_context!r}",
            )
        for rule in rules_for_context:
            state_changes = sum(
                value is not None
                for value in (rule.push, rule.pop)
            )
            if state_changes > 1:
                raise RuleError(
                    rule.line_no,
                    f"builder so activa uma unica mudanca de estado"
                    f" por regra: {rule.kinds[0]}",
                )

    if "builder" in parser_fields and not builder_contexts:
        raise RuleError(
            1,
            "builder em [parser] exige pelo menos um context com builder=true",
        )

    required = {"input", "output", "diagnostic", "tokenStream"}
    missing = sorted(required - set(parser_fields))
    if missing:
        raise RuleError(1, f"[parser] falta declarar: {', '.join(missing)}")

    return ParserRules(
        input=parser_fields["input"][0],
        output=parser_fields["output"][0],
        diagnostic=parser_fields["diagnostic"][0],
        token_stream=parser_fields["tokenStream"][0],
        contexts=contexts,
        parents=context_parents,
        rules=rules,
        end=end,
        on_error=on_error,
        context_builders=context_builders,
        builder=parser_fields.get("builder", (None, 0))[0],
        state_members=state_fields_map or None,
    )


def is_prefix(first: list[str], second: list[str]) -> bool:
    if len(first) < len(second):
        return first == second[:len(first)]
    if len(second) < len(first):
        return second == first[:len(second)]
    return first == second


def validate_types(rules: ParserRules, types_path: Path) -> None:
    text = types_path.read_text(encoding="utf-8")
    declared: set[str] = set()
    for keyword in ("struct", "class", "enum", "typedef", "using"):
        declared.update(
            name
            for name in re.findall(rf"\b{keyword}\s+([A-Za-z_]\w*)", text)
        )

    def type_names(cpp_type: str) -> list[str]:
        cleaned = re.sub(r"<.*>", "", cpp_type)
        cleaned = re.sub(r"&|\*|\bconst\b|\bvolatile\b", "", cleaned).strip()
        cleaned = cleaned.split("=", 1)[0].strip()
        if not cleaned:
            return []
        return list(filter(None, re.split(r"[^\w:]+", cleaned)))

    builtin = {
        "void", "bool", "char", "short", "int", "long", "float", "double",
        "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t", "std", "string",
        "string_view", "vector", "span", "TokenStream", "Token",
    }
    for cpp_type in (rules.input, rules.output, rules.diagnostic, rules.token_stream):
        for required in type_names(cpp_type):
            simple = required.rsplit(":", 1)[-1]
            if simple and simple not in declared and simple not in builtin:
                raise RuleError(
                    1,
                    f"tipo inexistente em types.hpp: {cpp_type!r}",
                )


def state_enum_lines(states: list[str]) -> list[str]:
    lines = ["enum class ParserState {"]
    for index, state in enumerate(states):
        end = "," if index + 1 < len(states) else ""
        lines.append(f"    {state}{end}")
    lines.append("};")
    return lines


def state_frame_decls(rules: ParserRules) -> list[str]:
    if not rules.state_members:
        return []
    lines = []
    for state in rules.states:
        lines.append(f"struct StateFrame_{state};")
        lines.append("")
    for state in rules.states:
        lines.append(f"struct StateFrame_{state} {{")
        if rules.state_members.get(state):
            for field in rules.state_members[state]:
                lines.append(f"    {field.cpp_type} {field.name}{{}};")
        else:
            lines.append("    std::monostate unused{};")
        lines.append("};")
        lines.append("")
    lines.extend(
        [
            "struct StateFrame {",
            "    ParserState state = ParserState::TopLevel;",
            "    std::variant<",
        ]
    )
    lines.append("        std::monostate,")
    for index, state in enumerate(rules.states):
        suffix = "," if index + 1 < len(rules.states) else ">"
        lines.append(f"        StateFrame_{state}{suffix}")
    lines.extend(
        [
            "    data;",
            "};",
            "",
            "template <ParserState State>",
            "struct StateFrameFor;",
        ]
    )
    for state in rules.states:
        lines.append(
            f"template <> struct StateFrameFor<ParserState::{state}> "
            f"{{ using type = StateFrame_{state}; }};"
        )
    lines.append("")
    return lines


def state_frame_accessors(rules: ParserRules, indent: str = "    ") -> list[str]:
    if not rules.state_members:
        return []
    lines = [
        f"{indent}template <ParserState State>",
        f"{indent}[[nodiscard]] bool hasFrame() const noexcept;",
        f"{indent}template <ParserState State>",
        f"{indent}[[nodiscard]] const StateFrameFor<State>::type &frame() const noexcept;",
        f"{indent}template <ParserState State>",
        f"{indent}[[nodiscard]] StateFrameFor<State>::type &frame() noexcept;",
        "",
    ]
    return lines


def state_frame_impls(rules: ParserRules, indent: str = "    ") -> list[str]:
    if not rules.state_members:
        return []
    out: list[str] = [
        f"{indent}template <typename Output>",
        f"{indent}template <ParserState State>",
        f"{indent}bool Parser<Output>::hasFrame() const noexcept {{",
        f"{indent}    for (const StateFrame &f : stack_)",
        f"{indent}        if (f.state == State)",
        f"{indent}            return true;",
        f"{indent}    return false;",
        f"{indent}}}",
        f"{indent}template <typename Output>",
        f"{indent}template <ParserState State>",
        f"{indent}const typename StateFrameFor<State>::type &",
        f"{indent}    Parser<Output>::frame() const noexcept {{",
        f"{indent}    for (const StateFrame &f : stack_)",
        f"{indent}        if (f.state == State)",
        f"{indent}            return std::get<typename StateFrameFor<State>::type>(f.data);",
        f"{indent}    std::abort();",
        f"{indent}}}",
        f"{indent}template <typename Output>",
        f"{indent}template <ParserState State>",
        f"{indent}typename StateFrameFor<State>::type &",
        f"{indent}    Parser<Output>::frame() noexcept {{",
        f"{indent}    for (StateFrame &f : stack_)",
        f"{indent}        if (f.state == State)",
        f"{indent}            return std::get<typename StateFrameFor<State>::type>(f.data);",
        f"{indent}    std::abort();",
        f"{indent}}}",
    ]
    return out


def state_frame_internal_fns(rules: ParserRules, indent: str = "    ") -> list[str]:
    if not rules.state_members:
        return []
    lines = [
        f"{indent}void resetInternal() noexcept {{",
        f"{indent}    stack_.clear();",
        f"{indent}    pushState(ParserState::TopLevel);",
        f"{indent}}}",
        f"{indent}[[nodiscard]] StateFrame makeFrame(ParserState state) noexcept {{",
        f"{indent}    switch (state) {{",
    ]
    for state in rules.states:
        lines.append(f"{indent}    case ParserState::{state}:")
        lines.append(f"{indent}        return StateFrame{{state, StateFrame_{state}{{}}}};")
    lines.extend(
        [
            f"{indent}    default:",
            f"{indent}        return StateFrame{{state, std::monostate{{}}}};",
            f"{indent}    }}",
            f"{indent}}}",
        ]
    )
    return lines


def context_conditions(context: str, parents: list[str], frames: bool) -> list[str]:
    cond = [f"topState() == ParserState::{context}"]
    for depth, parent in enumerate(parents, start=2):
        expr = f"stack_[stack_.size() - {depth}]"
        if frames:
            expr += ".state"
        cond.append(f"{expr} == ParserState::{parent}")
    return cond


def rule_conditions(rule: Rule, parents: list[str], frames: bool) -> list[str]:
    if len(rule.kinds) == 1:
        kind_cond = f"token.kind == TokenKind::{rule.kinds[0]}"
    else:
        kind_cond = "(" + " || ".join(
            f"token.kind == TokenKind::{kind}" for kind in rule.kinds
        ) + ")"
    cond: list[str] = [kind_cond]
    if rule.lexeme is not None:
        cond.append(f"this->lexeme(token) == {cpp_string(rule.lexeme)}")
    if rule.punc is not None:
        cond.append(f"token.punc == {cpp_char(rule.punc)}")
    cond.extend(context_conditions(rule.context, parents, frames))
    return cond


def make_rule_apply(rules: ParserRules) -> str:
    lines: list[str] = []
    if rules.end is not None:
        end_token = rules.end.rsplit("::", 1)[-1]
        lines.append(
            "        if (token.kind == TokenKind::"
            f"{end_token}) {{"
        )
        lines.append("            tokenStream().advance();")
        lines.append("            return true;")
        lines.append("        }")
    for rule in rules.rules:
        lines.append(
            f"        if ({' && '.join(rule_conditions(rule, rule.parents, bool(rules.state_members)))}) {{"
        )
        if rule.action is not None:
            lines.append(f"            {rule.action.qualified_name}(*this, token);")
        if rule.push is not None:
            lines.append(f"            pushState(ParserState::{rule.push});")
        if rule.pop is not None:
            lines.append(f"            if (!popState(ParserState::{rule.pop})) {{")
            lines.append(
                f"                diag(token.span, "
                f'"invalid pop {rule.pop}");'
            )
            lines.append("                return false;")
            lines.append("            }")
        lines.append("            tokenStream().advance();")
        lines.append("            return true;")
        lines.append("        }")
    if rules.on_error is not None:
        lines.append("        if (abort_)")
        lines.append("            return false;")
        lines.append(
            f"        switch ({rules.on_error.qualified_name}(*this, token)) {{"
        )
        lines.append("        case generated_parser::Recovery::Skip:")
        lines.append("            tokenStream().advance();")
        lines.append("            return true;")
        lines.append("        case generated_parser::Recovery::Abort:")
        lines.append("            abort_ = true;")
        lines.append("            diag(token.span, std::string(\"unexpected token \") +")
        lines.append("                             std::string(tokenKindName(token.kind)));")
        lines.append("            return false;")
        lines.append("        }")
        lines.append("        return false;")
        return "\n".join(lines)
    lines.append('        diag(token.span, std::string("unexpected token ") +')
    lines.append("                         std::string(tokenKindName(token.kind)));")
    lines.append("        return false;")
    return "\n".join(lines)


def make_actions_header(rules: ParserRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "frontend/parser/parser.hpp"',
        "",
    ]
    if rules.actions or rules.on_error is not None:
        lines.append("using Parser = generated_parser::Parser<"
                     f"{rules.output}>;")
        lines.append("using Token = generated_lexer::Token;")
        if rules.on_error is not None:
            lines.append("using Recovery = generated_parser::Recovery;")
        lines.append("")
    for hook in rules.actions:
        if hook.namespace:
            lines.append(
                f"namespace {hook.namespace} {{ "
                f"{hook.declaration(rules.output)} }}"
            )
        else:
            lines.append(hook.declaration(rules.output))
    lines.append("")
    return "\n".join(lines)


def make_parser_impl(rules: ParserRules) -> str:
    lines = [
        '#include "frontend/parser/parser.hpp"',
        "",
        "namespace generated_parser {",
    ]
    lines.append("")
    lines.append("} // namespace generated_parser")
    lines.append("")
    return "\n".join(lines)


def make_parser_header(rules: ParserRules) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/memory/dyn-array.hpp"',
        '#include "common/memory/optional.hpp"',
        '#include "common/memory/result.hpp"',
        '#include "frontend/lexer/lexer.hpp"',
        '#include "types.hpp"',
    ]
    if rules.builder_enabled:
        lines.append("")
        lines.append('#include "common/parser/builder.hpp"')
    lines.extend(
        [
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "#include <span>",
            "#include <string>",
            "#include <string_view>",
            "#include <utility>",
        ]
    )
    if rules.state_members:
        lines.append('#include <variant>')
    lines.extend(
        [
            "",
            "namespace generated_parser {",
            "",
            "template <typename Output> class Parser;",
            "",
            "enum class Recovery {",
            "    Skip,",
            "    Abort,",
            "};",
            "",
            "} // namespace generated_parser",
            "",
        ]
    )
    if rules.actions or rules.on_error is not None:
        lines.append("using Parser = generated_parser::Parser<"
                     f"{rules.output}>;")
        lines.append("using Token = generated_lexer::Token;")
        if rules.on_error is not None:
            lines.append("using Recovery = generated_parser::Recovery;")
        lines.append("")
        if rules.on_error is not None:
            lines.append("namespace hooks::parser {")
            lines.append("Recovery recover(Parser &parser, const Token &token);")
            lines.append("} // namespace hooks::parser")
            lines.append("")
    for hook in rules.actions:
        if hook.namespace:
            lines.append(
                f"namespace {hook.namespace} {{ "
                f"{hook.declaration(rules.output)} }}"
            )
        else:
            lines.append(hook.declaration(rules.output))
    if rules.actions:
        lines.append("")
    lines.extend(
        [
            "namespace generated_parser {",
            "",
        ]
    )
    lines.extend(state_enum_lines(rules.states))
    lines.extend(
        [
            "",
            "inline const char *parserStateName(ParserState state) noexcept {",
            "    switch (state) {",
        ]
    )
    for state in rules.states:
        lines.append(f'    case ParserState::{state}: return "{state}";')
    lines.extend(
        [
            '    default: return "?";',
            "    }",
            "}",
            "",
        ]
    )
    lines.extend(state_frame_decls(rules))
    lines.extend(make_parser_lookup(rules))
    lines.extend(
        [
            "template <typename T>",
            "concept ParserDiagnostic = requires(const T &diag) {",
            "    diag.span;",
            "    diag.message;",
            "};",
            "",
            "template <typename Output>",
            "class Parser {",
            "public:",
            f"    using Input = {rules.input};",
            "    using OutputAlias = Output;",
            f"    using DiagnosticAlias = {rules.diagnostic};",
            f"    using TokenStreamAlias = {rules.token_stream};",
        ]
    )
    if rules.builder_enabled:
        lines.append(f"    using BuilderAlias = {rules.builder_type};")
    lines.extend(
        [
            "    using TokenKind = generated_lexer::TokenKind;",
            "    static_assert(std::default_initializable<OutputAlias>,",
            '                  "parser output must be default constructible");',
            "    static_assert(ParserDiagnostic<DiagnosticAlias>,",
            '                  "parser diagnostic must expose span and message");',
            "",
            f"    explicit Parser(common::memory::Arena &arena)",
            f"        : stack_(arena), diagnostics_(arena)"
            + (", builder_(arena)" if rules.builder_enabled else "") + " {",
            ("        resetInternal();" if rules.state_members else
             "        pushState(ParserState::TopLevel);"),
            "    }",
            "",
        ]
    )
    lines.extend(
        [
            "    common::memory::Result<OutputAlias, DiagnosticAlias> parse(",
            "        TokenStreamAlias &tokens, Input source = Input{}) {",
            "        reset(tokens, source);",
            "        while (tokenStream().hasNext()) {",
            "            const generated_lexer::Token &token = current();",
            "            if (!step(token))",
            "                break;",
            "        }",
            "        if (abort_)",
            "            return DiagnosticAlias{diagnostics_.back()};",
            "        if (error())",
            "            return DiagnosticAlias{diagnostics_.back()};",
            "        if (!hasOutput())",
            "            return OutputAlias{};",
            "        return std::move(output_).value();",
            "    }",
            "",
            "    void reset(TokenStreamAlias &tokens, Input source = Input{}) {",
            "        tokens_ = &tokens;",
            "        source_ = source;",
            "        tokens.reset();",
            "        output_.reset();",
            "        diagnostics_.clear();",
            "        abort_ = false;",
            "        stack_.clear();",
            ("        resetInternal();" if rules.state_members else
             "        pushState(ParserState::TopLevel);"),
            "    }",
            "",
            "    [[nodiscard]] TokenStreamAlias &tokenStream() noexcept { return *tokens_; }",
            "    [[nodiscard]] const TokenStreamAlias &tokenStream() const noexcept { return *tokens_; }",
            "",
            "    [[nodiscard]] const generated_lexer::Token &current() const noexcept {",
            "        return tokenStream().current();",
            "    }",
            "    [[nodiscard]] const generated_lexer::Token &peek() const noexcept {",
            "        return tokenStream().peek();",
            "    }",
            "",
            "    [[nodiscard]] std::string_view lexeme(",
            "        const generated_lexer::Token &token) const noexcept {",
            "        if (!source_.empty() && token.span.end <= source_.size())",
            "            return span_slice(token.span);",
            "        return tokenStream().lexeme(token);",
            "    }",
            "    [[nodiscard]] std::string_view lexeme() const noexcept {",
            "        return lexeme(current());",
            "    }",
            "    [[nodiscard]] std::string_view span_slice(const Span &span) const noexcept {",
            "        if (source_.empty() || span.end > source_.size() ||",
            "            span.start > span.end)",
            "            return {};",
            "        return source_.substr(span.start, span.end - span.start);",
            "    }",
            "    [[nodiscard]] std::span<const generated_lexer::Token> slice(",
            "        size_t start, size_t count) const noexcept {",
            "        return tokenStream().slice(start, count);",
            "    }",
            "",
            ("    [[nodiscard]] ParserState topState() const noexcept {\n"
             "        return stack_.back().state;\n"
             "    }\n"
             "    [[nodiscard]] common::memory::DynArray<StateFrame> &stack() noexcept {\n"
             "        return stack_;\n"
             "    }\n"
             "    [[nodiscard]] const common::memory::DynArray<StateFrame> &stack() const noexcept {\n"
             "        return stack_;\n"
             "    }\n"
             "    void pushState(ParserState state) noexcept {\n"
             "        stack_.push(makeFrame(state));\n"
             "    }\n"
             if rules.state_members else
             "    [[nodiscard]] ParserState topState() const noexcept {\n"
             "        return stack_.back();\n"
             "    }\n"
             "    [[nodiscard]] common::memory::DynArray<ParserState> &stack() noexcept {\n"
             "        return stack_;\n"
             "    }\n"
             "    [[nodiscard]] const common::memory::DynArray<ParserState> &stack() const noexcept {\n"
             "        return stack_;\n"
             "    }\n"
             "    void pushState(ParserState state) noexcept {\n"
             "        stack_.push(state);\n"
             "    }\n"
             ),
            "    bool popState(ParserState expected) noexcept {",
            "        if (stack_.empty() || stack_.size() == 1 || topState() != expected)",
            "            return false;",
            "        popState();",
            "        return true;",
            "    }",
            "    void popState() noexcept {",
            "        if (stack_.size() > 1)",
            "            stack_.pop_back();",
            "    }",
            "",
            "    [[nodiscard]] OutputAlias &output() noexcept {",
            "        return output_.value();",
            "    }",
            "    [[nodiscard]] const OutputAlias &output() const noexcept {",
            "        return output_.value();",
            "    }",
            "    [[nodiscard]] bool hasOutput() const noexcept {",
            "        return output_.isValid();",
            "    }",
            "    void setOutput(OutputAlias value) {",
            "        output_ = std::move(value);",
            "    }",
            (f"""    [[nodiscard]] BuilderAlias &builder() noexcept {{
        return builder_;
    }}
    [[nodiscard]] const BuilderAlias &builder() const noexcept {{
        return builder_;
    }}
"""
             if rules.builder_enabled else ""),
            "    void diag(const Span &span, std::string_view message) {",
            "        DiagnosticAlias diagnostic = {};",
            "        diagnostic.span = span;",
            "        diagnostic.message = std::string(message);",
            "        diagnostics_.push(std::move(diagnostic));",
            "    }",
            "    [[nodiscard]] common::memory::DynArray<DiagnosticAlias> &diagnostics() noexcept {",
            "        return diagnostics_;",
            "    }",
            "    [[nodiscard]] const common::memory::DynArray<DiagnosticAlias> &diagnostics() const noexcept {",
            "        return diagnostics_;",
            "    }",
            "    [[nodiscard]] bool error() const noexcept {",
            "        return !diagnostics_.empty();",
            "    }",
            "",
            "    void abort() noexcept {",
            "        abort_ = true;",
            "    }",
            "",
            "    [[nodiscard]] ParserState lookupState(",
            "        std::string_view state) const noexcept {",
            "        return generated_parser::lookupState(state);",
            "    }",
            "",
        ]
    )
    if rules.state_members:
        lines.extend(state_frame_accessors(rules))
    lines.extend(
        [
            "",
            "    bool step(const generated_lexer::Token &token) {",
            "        (void)token;",
            make_rule_apply(rules),
            "    }",
            "",
            "private:",
            "    TokenStreamAlias *tokens_ = nullptr;",
            "    Input source_{};",
            ("    common::memory::DynArray<StateFrame> stack_;"
             if rules.state_members else
             "    common::memory::DynArray<ParserState> stack_;"),
            "    common::memory::DynArray<DiagnosticAlias> diagnostics_;",
            "    common::memory::Optional<OutputAlias> output_;",
            ("    BuilderAlias builder_;"
             if rules.builder_enabled else ""),
            "    bool abort_ = false;",
            *state_frame_internal_fns(rules),
            "};",
            "",
        ]
    )
    if rules.state_members:
        lines.extend(state_frame_impls(rules, indent=""))
        lines.append("")
        lines.append("} // namespace generated_parser")
        lines.append("")
    else:
        lines.append("} // namespace generated_parser")
        lines.append("")
    return "\n".join(lines)


def make_parser_lookup(rules: ParserRules) -> list[str]:
    lines = ["[[nodiscard]] inline ParserState lookupState("]
    lines.append("    std::string_view state) noexcept {")
    lines.append('    if (state == "TopLevel") return ParserState::TopLevel;')
    for state in rules.states:
        if state != "TopLevel":
            lines.append(f'    if (state == "{state}") return ParserState::{state};')
    lines.append("    return ParserState::TopLevel;")
    lines.append("}")
    lines.append("")
    return lines


def make_gitignore() -> str:
    return gitignore_lines(["parser.hpp", "parser.cpp", "actions.hpp"])


def generated_files(rules: ParserRules) -> list[tuple[str, str]]:
    return [
        ("parser.hpp", make_parser_header(rules)),
        ("parser.cpp", make_parser_impl(rules)),
        ("actions.hpp", make_actions_header(rules)),
        (".gitignore", make_gitignore()),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/frontend/parser/parser.rules")
    parser.add_argument("--out", default="build/src/frontend/parser")
    parser.add_argument(
        "--types",
        default="src/frontend/parser/types.hpp",
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
        rules = parse_rules(rules_path.read_text(encoding="utf-8"), rules_path)
        validate_types(rules, types_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    written = write_generated_files(out_path, generated_files(rules))
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
