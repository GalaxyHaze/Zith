#!/usr/bin/env python3
"""Generate cli.hpp, cli.cpp, actions.hpp and .gitignore from cli.rules."""

from __future__ import annotations

import argparse
import ast
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


@dataclass
class Action:
    qualified_name: str
    takes_opts: bool
    line_no: int

    @property
    def namespace(self) -> str:
        parts = self.qualified_name.rsplit("::", 1)
        return parts[0] if len(parts) == 2 else ""

    @property
    def function_name(self) -> str:
        return self.qualified_name.rsplit("::", 1)[-1]

    @property
    def declaration(self) -> str:
        if self.takes_opts:
            return f"int {self.function_name}(const generated_cli::Options &opts);"
        return f"int {self.function_name}();"

    @property
    def call_expr(self) -> str:
        if self.takes_opts:
            return f"{self.qualified_name}(options)"
        return f"{self.qualified_name}()"


@dataclass
class Flag:
    names: list[str]
    kind: str
    line_no: int
    default: object | None = None
    choices: list[str] | None = None
    separator: str = ","
    min_value: int | None = None
    max_value: int | None = None
    action: Action | None = None

    @property
    def canonical_name(self) -> str:
        long_names = [name for name in self.names if len(name) > 1]
        return long_names[0] if long_names else self.names[0]

    @property
    def member_name(self) -> str:
        if self.canonical_name == "include":
            return "includeDirs"
        return to_camel(self.canonical_name)


@dataclass
class Arg:
    name: str
    qualified_name: str
    kind: str
    line_no: int
    variadic: bool = False
    choices: list[str] | None = None
    default: object | None = None
    has_default: bool = False

    @property
    def member_name(self) -> str:
        return to_camel(self.name)


@dataclass
class Subcommand:
    name: str
    line_no: int
    action: Action | None = None
    flags: list[Flag] = field(default_factory=list)
    args: list[Arg] = field(default_factory=list)

    @property
    def enum_name(self) -> str:
        return to_pascal(self.name)

    @property
    def member_name(self) -> str:
        return to_camel(self.name)

    @property
    def options_type_name(self) -> str:
        return f"{to_pascal(self.name)}Options"


@dataclass
class Command:
    name: str
    line_no: int
    action: Action | None = None
    flags: list[Flag] = field(default_factory=list)
    args: list[Arg] = field(default_factory=list)
    subcommands: dict[str, Subcommand] = field(default_factory=dict)

    @property
    def enum_name(self) -> str:
        return to_pascal(self.name)

    @property
    def member_name(self) -> str:
        return to_camel(self.name)

    @property
    def options_type_name(self) -> str:
        return f"{to_pascal(self.name)}Options"

    @property
    def subcommand_enum_name(self) -> str:
        return f"{to_pascal(self.name)}Subcommand"

    @property
    def subcommand_member_name(self) -> str:
        return f"{self.member_name}Subcommand"

    @property
    def is_container(self) -> bool:
        return bool(self.subcommands) and self.action is None


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


def to_camel(name: str) -> str:
    parts = [part for part in re.split(r"[-_]", name) if part]
    if not parts:
        return name
    return parts[0] + "".join(part.capitalize() for part in parts[1:])


def to_pascal(name: str) -> str:
    return "".join(part.capitalize() for part in re.split(r"[-_]", name) if part)


def cpp_string(value: str) -> str:
    return json.dumps(value)


def split_names(raw: str, line_no: int) -> list[str]:
    names = [name.strip() for name in raw.split("|") if name.strip()]
    if not names:
        raise RuleError(line_no, f"flag sem nomes: {raw!r}")
    return names


def parse_action(text: str, line_no: int) -> Action:
    match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_:]*)\((.*)\)", text.strip())
    if not match:
        raise RuleError(line_no, f"forma de acao invalida: {text!r}")

    qualified_name = match.group(1)
    raw_args = match.group(2).strip()
    if not raw_args:
        return Action(qualified_name=qualified_name, takes_opts=False, line_no=line_no)
    if raw_args == "opts":
        return Action(qualified_name=qualified_name, takes_opts=True, line_no=line_no)
    raise RuleError(line_no, f"acao so pode usar nenhum argumento ou 'opts': {text!r}")


def parse_flag(text: str, line_no: int) -> Flag:
    if "->" in text:
        lhs, rhs = text.split("->", 1)
        return Flag(
            names=split_names(lhs.strip(), line_no),
            kind="action",
            line_no=line_no,
            action=parse_action(rhs.strip(), line_no),
        )

    if "=" not in text:
        raise RuleError(line_no, f"flag invalida: {text!r}")

    lhs, rhs = text.split("=", 1)
    names = split_names(lhs.strip(), line_no)
    rhs = rhs.strip()

    choices_match = re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_-]*(?:\s*\|\s*[A-Za-z_][A-Za-z0-9_-]*)+",
        rhs,
    )
    if choices_match:
        choices = [choice.strip() for choice in rhs.split("|")]
        return Flag(
            names=names,
            kind="enum",
            line_no=line_no,
            choices=choices,
            default=choices[0],
        )

    if rhs.startswith("[]"):
        separator = ","
        if "/" in rhs:
            _, tail = rhs.split("/", 1)
            tail = tail.strip()
            if tail:
                separator = tail[0]
        return Flag(names=names, kind="list", line_no=line_no, separator=separator)

    range_match = re.fullmatch(r"([+-]?\d+)\s*\.\.\s*([+-]?\d+)", rhs)
    if range_match:
        return Flag(
            names=names,
            kind="range",
            line_no=line_no,
            min_value=int(range_match.group(1)),
            max_value=int(range_match.group(2)),
        )

    lowered = rhs.lower()
    if lowered in {"true", "false"}:
        return Flag(names=names, kind="bool", line_no=line_no, default=(lowered == "true"))

    if re.fullmatch(r"\+?\d+", rhs):
        return Flag(names=names, kind="int", line_no=line_no, default=int(rhs))

    if (rhs.startswith('"') and rhs.endswith('"')) or (rhs.startswith("'") and rhs.endswith("'")):
        try:
            value = ast.literal_eval(rhs)
        except (ValueError, SyntaxError) as exc:
            raise RuleError(line_no, f"string invalida: {rhs!r}") from exc
        if not isinstance(value, str):
            raise RuleError(line_no, f"default string invalido: {rhs!r}")
        return Flag(names=names, kind="string", line_no=line_no, default=value)

    return Flag(names=names, kind="string", line_no=line_no, default=rhs)


def parse_arg(text: str, line_no: int) -> Arg:
    lhs, rhs = text.split(":", 1)
    qualified_name = lhs.strip()
    name = qualified_name.split(".")[-1].strip()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", name):
        raise RuleError(line_no, f"nome de argumento invalido: {lhs!r}")

    rhs = rhs.strip()
    variadic = False
    has_default = False
    default: object | None = None

    if "=" in rhs:
        raw_type, raw_default = rhs.split("=", 1)
        has_default = True
    else:
        raw_type = rhs
        raw_default = ""

    raw_type = raw_type.strip()
    if raw_type.endswith("..."):
        variadic = True
        raw_type = raw_type[:-3].strip()

    if raw_type in {"string", "path"}:
        kind = raw_type
        if has_default:
            default = ast.literal_eval(raw_default.strip())
            if not isinstance(default, str):
                raise RuleError(line_no, f"default de {raw_type} deve ser string: {raw_default!r}")
    elif raw_type == "int":
        kind = "int"
        if has_default:
            default_text = raw_default.strip()
            if not re.fullmatch(r"[+-]?\d+", default_text):
                raise RuleError(line_no, f"default de int invalido: {raw_default!r}")
            default = int(default_text)
    elif raw_type == "bool":
        kind = "bool"
        if has_default:
            lowered = raw_default.strip().lower()
            if lowered not in {"true", "false"}:
                raise RuleError(line_no, f"default de bool deve ser true/false: {raw_default!r}")
            default = lowered == "true"
    elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*(?:\s*\|\s*[A-Za-z_][A-Za-z0-9_-]*)+",
                      raw_type):
        kind = "enum"
        choices = [choice.strip() for choice in raw_type.split("|")]
        if has_default:
            default = raw_default.strip()
            if default not in choices:
                raise RuleError(
                    line_no,
                    f"default de enum deve pertencer a {choices!r}: {raw_default!r}",
                )
    else:
        raise RuleError(line_no, f"tipo de argumento invalido: {raw_type!r}")

    if variadic and has_default:
        raise RuleError(line_no, f"argumento variadico nao pode ter default: {text!r}")

    return Arg(
        name=name,
        qualified_name=qualified_name,
        kind=kind,
        line_no=line_no,
        variadic=variadic,
        choices=choices if kind == "enum" else None,
        default=default,
        has_default=has_default,
    )


def ensure_command(commands: dict[str, Command], name: str, line_no: int) -> Command:
    command = commands.get(name)
    if command is None:
        command = Command(name=name, line_no=line_no)
        commands[name] = command
    return command


def ensure_subcommand(command: Command, name: str, line_no: int) -> Subcommand:
    subcommand = command.subcommands.get(name)
    if subcommand is None:
        subcommand = Subcommand(name=name, line_no=line_no)
        command.subcommands[name] = subcommand
    return subcommand


def parse_rules(
    text: str,
    path: Path,
) -> tuple[list[Flag], dict[str, Command], list[Action]]:
    section = "flags"
    flags: list[Flag] = []
    global_args: list[Arg] = []
    commands: dict[str, Command] = {}
    actions_by_name: dict[str, Action] = {}
    last_subcommand_parent: str | None = None
    last_subflag_prefix: tuple[str, str | None] | None = None

    def register_action(action: Action) -> None:
        existing = actions_by_name.get(action.qualified_name)
        if existing is None:
            actions_by_name[action.qualified_name] = action
            return
        if existing.takes_opts != action.takes_opts:
            raise RuleError(
                action.line_no,
                f"assinatura inconsistente para {action.qualified_name!r}",
            )

    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue

        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            section = header.group(1).strip().lower()
            last_subcommand_parent = None
            last_subflag_prefix = None
            continue

        if section == "flags":
            flag = parse_flag(line, line_no)
            flags.append(flag)
            if flag.action is not None:
                register_action(flag.action)
            continue

        if section == "commands":
            match = re.fullmatch(r"([A-Za-z0-9_-]+)(?:\s*->\s*(.+))?", line)
            if not match:
                raise RuleError(line_no, f"comando invalido: {line!r}")
            name = match.group(1)
            if name in commands:
                raise RuleError(line_no, f"comando repetido: {name!r}")
            action_text = match.group(2)
            action = parse_action(action_text, line_no) if action_text else None
            if action is not None:
                register_action(action)
            commands[name] = Command(name=name, line_no=line_no, action=action)
            continue

        if section == "args":
            arg = parse_arg(line, line_no)
            if arg.qualified_name != arg.name:
                raise RuleError(line_no, f"argumento global com qualificador invalido: {line!r}")
            global_args.append(arg)
            continue

        if section == "command-args":
            arg = parse_arg(line, line_no)
            parts = arg.qualified_name.split(".")
            if len(parts) == 2:
                command = ensure_command(commands, parts[0], line_no)
                if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", arg.name):
                    raise RuleError(line_no, f"nome de argumento invalido: {line!r}")
                command.args.append(arg)
            elif len(parts) == 3:
                command = ensure_command(commands, parts[0], line_no)
                if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", arg.name):
                    raise RuleError(line_no, f"nome de argumento invalido: {line!r}")
                ensure_subcommand(command, parts[1], line_no).args.append(arg)
            else:
                raise RuleError(
                    line_no,
                    f"qualificador de argumento invalido: {arg.qualified_name!r}",
                )
            continue

        if section == "sub-commands":
            match = re.fullmatch(
                r"(?:(?P<parent>[A-Za-z0-9_-]+)\s*:\s*)?(?P<child>[A-Za-z0-9_-]+)"
                r"(?:\s*->\s*(?P<action>.+))?",
                line,
            )
            if not match:
                raise RuleError(line_no, f"subcomando invalido: {line!r}")
            parent = match.group("parent") or last_subcommand_parent
            if not parent:
                raise RuleError(line_no, f"subcomando sem pai explicito: {line!r}")
            child = match.group("child")
            action_text = match.group("action")
            command = ensure_command(commands, parent, line_no)
            action = parse_action(action_text, line_no) if action_text else None
            if action is not None:
                register_action(action)
            existing = command.subcommands.get(child)
            if existing is None:
                command.subcommands[child] = Subcommand(
                    name=child,
                    line_no=line_no,
                    action=action,
                )
            else:
                if existing.action is not None and action is not None:
                    if (existing.action.qualified_name != action.qualified_name
                            or existing.action.takes_opts != action.takes_opts):
                        raise RuleError(
                            line_no,
                            f"acao inconsistente para subcomando: {parent}:{child}",
                        )
                if action is not None:
                    existing.action = action
            last_subcommand_parent = parent
            continue

        if section == "sub-flags":
            prefix: tuple[str, str | None] | None = None
            flag_text = line
            if ":" in line:
                parts = [part.strip() for part in line.split(":")]
                if len(parts) >= 2 and ("=" in parts[-1] or "->" in parts[-1]):
                    flag_text = parts[-1]
                    prefix_parts = [part for part in parts[:-1] if part]
                    if len(prefix_parts) == 1:
                        prefix = (prefix_parts[0], None)
                    elif len(prefix_parts) == 2:
                        prefix = (prefix_parts[0], prefix_parts[1])
                    elif len(prefix_parts) > 2:
                        raise RuleError(line_no, f"prefixo de sub-flag invalido: {line!r}")
            if prefix is None:
                prefix = last_subflag_prefix
            if prefix is None:
                raise RuleError(line_no, f"sub-flag sem contexto: {line!r}")

            command = ensure_command(commands, prefix[0], line_no)
            flag = parse_flag(flag_text.strip(), line_no)
            if flag.action is not None:
                register_action(flag.action)
            if prefix[1] is None:
                command.flags.append(flag)
            else:
                ensure_subcommand(command, prefix[1], line_no).flags.append(flag)
            last_subflag_prefix = prefix
            continue

        raise RuleError(line_no, f"secao desconhecida: [{section}]")

    def validate_args(arg_list: list[Arg], context: str) -> None:
        variadic_seen = False
        seen: set[str] = set()
        for index, arg in enumerate(arg_list):
            if arg.name in seen:
                raise RuleError(arg.line_no, f"argumento repetido no contexto {context}: {arg.name!r}")
            seen.add(arg.name)
            if arg.variadic:
                if variadic_seen:
                    raise RuleError(arg.line_no, f"contexto {context} tem mais de um variadico")
                variadic_seen = True
            if arg.variadic and index + 1 != len(arg_list):
                raise RuleError(arg.line_no, f"variadico deve ser o ultimo de {context}")

    validate_args(global_args, "global")
    for command in commands.values():
        validate_args(command.args, command.name)
        for subcommand in command.subcommands.values():
            validate_args(subcommand.args, f"{command.name}:{subcommand.name}")

    return flags, global_args, commands, list(actions_by_name.values())


def arg_enum_type_name(context: str, arg_name: str) -> str:
    return to_pascal(context) + to_pascal(arg_name)


def flag_enum_type_name(context: str, flag: Flag) -> str:
    return to_pascal(context) + to_pascal(flag.canonical_name)


def option_field_lines(
    flags: list[Flag],
    args: list[Arg],
    prefix: str,
    target: str,
) -> list[str]:
    lines: list[str] = []
    for flag in flags:
        member = flag.member_name
        if flag.kind == "bool":
            default = "true" if flag.default else "false"
            lines.append(f"    bool {member} = {default};")
        elif flag.kind == "int":
            lines.append(f"    int {member} = {flag.default};")
        elif flag.kind == "range":
            lines.append(f"    int {member} = {flag.min_value};")
        elif flag.kind == "string":
            lines.append(f"    InternId {member};")
        elif flag.kind == "enum":
            default = to_pascal(str(flag.default))
            enum_name = flag_enum_type_name(prefix, flag)
            lines.append(f"    {enum_name} {member} = {enum_name}::{default};")
        elif flag.kind == "list":
            lines.append(f"    zith::memory::DynArray<InternId> {member};")
        elif flag.kind == "action":
            continue
        else:
            raise ValueError(f"unsupported flag kind in {target}: {flag.kind}")
    for arg in args:
        member = arg.member_name
        if arg.variadic:
            lines.append(f"    zith::memory::DynArray<{arg_cpp_type(arg, prefix)}> {member};")
        elif arg.kind == "int":
            default = arg.default if arg.has_default else 0
            lines.append(f"    int {member} = {default};")
        elif arg.kind == "bool":
            default = "true" if arg.has_default and arg.default else "false"
            lines.append(f"    bool {member} = {default};")
        elif arg.kind == "enum":
            enum_name = arg_enum_type_name(prefix, arg.name)
            default = arg.default if arg.has_default else (arg.choices or [""])[0]
            lines.append(
                f"    {enum_name} {member} = {enum_name}::{to_pascal(str(default))};"
            )
        else:
            lines.append(f"    {arg_cpp_type(arg, prefix)} {member};")
    return lines


def arg_cpp_type(arg: Arg, prefix: str) -> str:
    if arg.kind in {"string", "path"}:
        return "InternId"
    if arg.kind == "int":
        return "int"
    if arg.kind == "bool":
        return "bool"
    if arg.kind == "enum":
        return arg_enum_type_name(prefix, arg.name)
    raise ValueError(f"unsupported arg kind: {arg.kind}")


def option_init_lines(flags: list[Flag], args: list[Arg]) -> list[str]:
    init: list[str] = []
    for flag in flags:
        if flag.kind == "list":
            init.append(f"{flag.member_name}(arena)")
        elif flag.kind == "string":
            init.append(f"{flag.member_name}(strings.intern({cpp_string(str(flag.default))}))")
    for arg in args:
        if arg.variadic:
            init.append(f"{arg.member_name}(arena)")
        elif arg.kind in {"string", "path"}:
            init.append(
                f"{arg.member_name}(strings.intern({cpp_string(str(arg.default or ''))}))"
            )
    return init


def enum_definition_lines(name: str, choices: list[str]) -> list[str]:
    lines = [f"enum class {name} {{"]
    for choice in choices:
        lines.append(f"    {to_pascal(choice)},")
    lines.append("};")
    return lines


def _context_prefixes(
    commands: dict[str, Command],
    global_args: list[Arg],
    global_flags: list[Flag],
) -> list[tuple[str, list[Arg], list[Flag]]]:
    contexts: list[tuple[str, list[Arg], list[Flag]]] = [
        ("global", global_args, global_flags)
    ]
    for command in commands.values():
        contexts.append((command.name, command.args, command.flags))
        for subcommand in command.subcommands.values():
            contexts.append(
                (
                    f"{command.name}{to_pascal(subcommand.name)}",
                    subcommand.args,
                    subcommand.flags,
                )
            )
    return contexts


def make_actions_header(actions: list[Action]) -> str:
    lines = [
        "#pragma once",
        "",
        "namespace generated_cli {",
        "struct Options;",
        "}",
        "",
    ]
    for index, action in enumerate(actions):
        if action.namespace:
            lines.append(f"namespace {action.namespace} {{ {action.declaration} }}")
        else:
            lines.append(action.declaration)
        if index + 1 != len(actions):
            lines.append("")
    return "\n".join(lines) + "\n"


def option_struct_lines(
    type_name: str,
    context_name: str,
    flags: list[Flag],
    args: list[Arg],
    nested: list[tuple[str, str]],
) -> list[str]:
    lines = [f"struct {type_name} {{"]
    body = option_field_lines(flags, args, context_name, type_name)
    if body:
        lines.extend(body)
    for nested_type, member in nested:
        lines.append(f"    {nested_type} {member};")
    if not body and not nested:
        lines.append("    // no fields")
    init = option_init_lines(flags, args)
    for _, member in nested:
        init.append(f"{member}(arena, strings)")

    lines.append(
        f"    {type_name}(zith::memory::Arena &arena, "
        "zith::memory::StringInterner &strings)"
    )
    if init:
        lines.append("        : " + ",\n          ".join(init))
    lines.extend(
        [
            "    {",
            "        (void)arena;",
            "        (void)strings;",
            "    }",
            "};",
        ]
    )
    return lines


def make_cli_header(flags: list[Flag], commands: dict[str, Command], args: list[Arg]) -> str:
    lines = [
        "#pragma once",
        "",
        "#include \"common/arena.hpp\"",
        "#include \"common/dyn-array.hpp\"",
        "#include \"common/string-interner.hpp\"",
        "",
        "namespace generated_cli {",
        "",
        "using InternId = zith::memory::InternedId;",
        "",
        "",
    ]

    enum_defs: list[str] = []
    for context_name, context_args, context_flags in _context_prefixes(commands, args, flags):
        for arg in context_args:
            if arg.kind != "enum":
                continue
            enum_defs.extend(
                enum_definition_lines(arg_enum_type_name(context_name, arg.name), arg.choices or [])
            )
        for flag in context_flags:
            if flag.kind != "enum":
                continue
            enum_defs.extend(
                enum_definition_lines(
                    flag_enum_type_name(context_name, flag),
                    flag.choices or [],
                )
            )
    if enum_defs:
        lines.extend(enum_defs)
        lines.append("")

    for command in commands.values():
        for subcommand in command.subcommands.values():
            if subcommand.flags or subcommand.args:
                lines.extend(
                    option_struct_lines(
                        f"{to_pascal(command.name)}{subcommand.options_type_name}",
                        f"{command.name}{to_pascal(subcommand.name)}",
                        subcommand.flags,
                        subcommand.args,
                        [],
                    )
                )
                lines.append("")

        if command.flags or command.args or command.subcommands:
            nested = [
                (
                    f"{to_pascal(command.name)}{subcommand.options_type_name}",
                    subcommand.member_name,
                )
                for subcommand in command.subcommands.values()
                if subcommand.flags or subcommand.args
            ]
            lines.extend(
                option_struct_lines(
                    command.options_type_name,
                    command.name,
                    command.flags,
                    command.args,
                    nested,
                )
            )
            lines.append("")

    lines.append("enum class Command {")
    lines.append("    None,")
    for command in commands.values():
        lines.append(f"    {command.enum_name},")
    lines.append("};")
    lines.append("")

    for command in commands.values():
        if not command.subcommands:
            continue
        lines.append(f"enum class {command.subcommand_enum_name} {{")
        lines.append("    None,")
        for subcommand in command.subcommands.values():
            lines.append(f"    {subcommand.enum_name},")
        lines.append("};")
        lines.append("")

    options_lines = ["struct Options {"]
    global_fields = option_field_lines(flags, args, "global", "global")
    options_lines.extend(global_fields or ["    // no global flags"])
    command_init: list[str] = []
    for command in commands.values():
        if command.flags or command.args or any(
            sub.flags or sub.args for sub in command.subcommands.values()
        ):
            options_lines.append(f"    {command.options_type_name} {command.member_name};")
            command_init.append(f"{command.member_name}(arena, strings)")
    options_lines.append(
        "    zith::memory::StringInterner *stringPool = nullptr;"
    )
    options_lines.extend(
        ["    Options(zith::memory::Arena &arena, zith::memory::StringInterner &strings)"]
    )
    init: list[str] = option_init_lines(flags, args)
    init.extend(command_init)
    init.append("stringPool(&strings)")
    if init:
        options_lines.append("        : " + ",\n          ".join(init))
    options_lines.extend(["{", "        (void)arena;", "    }", "};"])
    lines.extend(options_lines)
    lines.append("")
    lines.append("struct Cli {")
    lines.append(
        "    zith::memory::Arena arena;"
    )
    lines.append(
        "    zith::memory::StringInterner strings;"
    )
    lines.append(
        "    Options options;"
    )
    lines.extend(
        [
            "    Cli()",
            "        : strings(arena),",
            "          options(arena, strings) {}",
        ]
    )
    lines.append("    Command command = Command::None;")
    for command in commands.values():
        if command.subcommands:
            lines.append(
                f"    {command.subcommand_enum_name} {command.subcommand_member_name} = "
                f"{command.subcommand_enum_name}::None;"
            )
    lines.append("")
    lines.append("    int parseArgs(int argc, char **argv);")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace generated_cli")
    return "\n".join(lines) + "\n"


def all_flag_forms(flag: Flag) -> list[str]:
    forms: list[str] = []
    for name in flag.names:
        form = f"-{name}" if len(name) == 1 else f"--{name}"
        if form not in forms:
            forms.append(form)
    return forms


def compare_expr(values: list[str]) -> str:
    args = ", ".join(cpp_string(value) for value in values)
    return f"compare(arg, {args})"


def emit_flag_handler(flag: Flag, target_expr: str, indent: str) -> str:
    compare = compare_expr(all_flag_forms(flag))
    member = f"{target_expr}.{flag.member_name}"
    if flag.kind == "action":
        return f"{indent}if ({compare}) return {flag.action.call_expr};"
    if flag.kind == "bool":
        return f"{indent}if ({compare}) {{ {member} = true; continue; }}"
    if flag.kind == "string":
        return (
            f"{indent}if ({compare}) {{\n"
            f"{indent}    if (i + 1 >= argc) return 2;\n"
            f"{indent}    {member} = strings.intern(argv[++i]);\n"
            f"{indent}    continue;\n"
            f"{indent}}}"
        )
    if flag.kind == "list":
        return (
            f"{indent}if ({compare}) {{\n"
            f"{indent}    if (i + 1 >= argc) return 2;\n"
            f"{indent}    appendSplit({member}, strings, argv[++i], "
            f"{cpp_string(flag.separator)}[0]);\n"
            f"{indent}    continue;\n"
            f"{indent}}}"
        )
    if flag.kind == "int":
        return (
            f"{indent}if ({compare}) {{\n"
            f"{indent}    if (i + 1 >= argc) return 2;\n"
            f"{indent}    long value = 0;\n"
            f"{indent}    if (!parseLong(argv[++i], value) || value <= 0) return 2;\n"
            f"{indent}    {member} = static_cast<int>(value);\n"
            f"{indent}    continue;\n"
            f"{indent}}}"
        )
    if flag.kind == "range":
        return (
            f"{indent}if ({compare}) {{\n"
            f"{indent}    if (i + 1 >= argc) return 2;\n"
            f"{indent}    long value = 0;\n"
            f"{indent}    if (!parseLong(argv[++i], value) || value < {flag.min_value} "
            f"|| value > {flag.max_value}) return 2;\n"
            f"{indent}    {member} = static_cast<int>(value);\n"
            f"{indent}    continue;\n"
            f"{indent}}}"
        )
    if flag.kind == "enum":
        enum_name = flag_enum_type_name(_context_name_from_target(target_expr), flag)
        lines = [f"{indent}if ({compare}) {{"]
        lines.append(f"{indent}    if (i + 1 >= argc) return 2;")
        lines.append(f"{indent}    const char *value = argv[++i];")
        for choice in flag.choices or []:
            lines.append(
                f"{indent}    if (compare(value, {cpp_string(choice)})) "
                f"{{ {member} = {enum_name}::{to_pascal(choice)}; continue; }}"
            )
        lines.append(f"{indent}    return 2;")
        lines.append(f"{indent}}}")
        return "\n".join(lines)
    raise ValueError(f"unsupported flag kind: {flag.kind}")


def emit_reset_lines(
    flags: list[Flag],
    args: list[Arg],
    prefix: str,
    indent: str,
) -> list[str]:
    lines: list[str] = []
    for flag in flags:
        member = f"{prefix}.{flag.member_name}"
        if flag.kind == "bool":
            lines.append(f"{indent}{member} = false;")
        elif flag.kind == "int":
            lines.append(f"{indent}{member} = {flag.default};")
        elif flag.kind == "range":
            lines.append(f"{indent}{member} = {flag.min_value};")
        elif flag.kind == "string":
            lines.append(
                f"{indent}{member} = strings.intern({cpp_string(str(flag.default))});"
            )
        elif flag.kind == "enum":
            enum_name = flag_enum_type_name(_context_name_from_prefix(prefix), flag)
            lines.append(
                f"{indent}{member} = {enum_name}::{to_pascal(str(flag.default))};"
            )
        elif flag.kind == "list":
            lines.append(f"{indent}{member}.clear();")
        elif flag.kind == "action":
            continue
        else:
            raise ValueError(f"unsupported flag kind in reset: {flag.kind}")
    for arg in args:
        member = f"{prefix}.{arg.member_name}"
        if arg.variadic:
            lines.append(f"{indent}{member}.clear();")
        elif arg.kind in {"string", "path"}:
            lines.append(
                f"{indent}{member} = strings.intern({cpp_string(str(arg.default or ''))});"
            )
        elif arg.kind == "int":
            lines.append(f"{indent}{member} = {arg.default if arg.has_default else 0};")
        elif arg.kind == "bool":
            lines.append(f"{indent}{member} = {'true' if arg.has_default and arg.default else 'false'};")
        elif arg.kind == "enum":
            enum_name = arg_enum_type_name(_context_name_from_prefix(prefix), arg.name)
            default = arg.default if arg.has_default else (arg.choices or [""])[0]
            lines.append(f"{indent}{member} = {enum_name}::{to_pascal(str(default))};")
        else:
            raise ValueError(f"unsupported arg kind in reset: {arg.kind}")
    return lines


def reset_statements(
    flags: list[Flag],
    args: list[Arg],
    commands: dict[str, Command],
) -> list[str]:
    lines: list[str] = []
    lines.extend(emit_reset_lines(flags, args, "options", "    "))
    for command in commands.values():
        if command.flags or command.args:
            lines.extend(
                emit_reset_lines(
                    command.flags,
                    command.args,
                    f"options.{command.member_name}",
                    "    ",
                )
            )
        for subcommand in command.subcommands.values():
            if subcommand.flags or subcommand.args:
                lines.extend(
                    emit_reset_lines(
                        subcommand.flags,
                        subcommand.args,
                        f"options.{command.member_name}.{subcommand.member_name}",
                        "    ",
                    )
                )
    return lines


def has_string_fields(flags: list[Flag], args: list[Arg]) -> bool:
    return any(flag.kind in {"string", "list"} for flag in flags) or any(
        arg.kind in {"string", "path"} for arg in args
    )


def _context_name_from_prefix(prefix: str) -> str:
    parts = [part for part in prefix.split(".") if part != "options"]
    if not parts:
        return "global"
    if len(parts) == 1:
        return to_pascal(parts[0]) if parts[0] != "global" else "global"
    return to_pascal(parts[0]) + to_pascal(parts[1])


def _context_name_from_target(target_expr: str) -> str:
    parts = [part for part in target_expr.split(".") if part != "options"]
    return "".join(to_pascal(part) for part in parts) if parts else "global"


def emit_arg_assignment(arg: Arg, target_expr: str, indent: str) -> list[str]:
    member = f"{target_expr}.{arg.member_name}"
    lines: list[str] = []

    if arg.kind in {"string", "path"}:
        if arg.variadic:
            lines.append(f"{indent}{member}.push(strings.intern(arg));")
        else:
            lines.append(f"{indent}{member} = strings.intern(arg);")
    elif arg.kind == "int":
        lines.append(f"{indent}long value = 0;")
        lines.append(
            f'{indent}if (!parseLong(arg, value)) '
            f'{{ std::fprintf(stderr, "invalid integer for '
            f'{_context_human(target_expr)}.{arg.name}: %s\\n", arg); return 2; }}'
        )
        if arg.variadic:
            lines.append(f"{indent}{member}.push(static_cast<int>(value));")
        else:
            lines.append(f"{indent}{member} = static_cast<int>(value);")
    elif arg.kind == "bool":
        target = f"{member}.push(true);" if arg.variadic else f"{member} = true;"
        lines.append(
            f'{indent}if (compare(arg, "true")) {{ {target} }}'
        )
        target = f"{member}.push(false);" if arg.variadic else f"{member} = false;"
        lines.append(
            f'{indent}else if (compare(arg, "false")) {{ {target} }}'
        )
        lines.append(
            f'{indent}else {{ std::fprintf(stderr, "invalid bool for '
            f'{_context_human(target_expr)}.{arg.name}: %s\\n", arg); return 2; }}'
        )
    elif arg.kind == "enum":
        enum_name = arg_enum_type_name(_context_name_from_target(target_expr), arg.name)
        lines.append(f"{indent}bool matched = false;")
        for choice in arg.choices or []:
            enum_value = f"{enum_name}::{to_pascal(choice)}"
            if arg.variadic:
                body = f"{member}.push({enum_value});"
            else:
                body = f"{member} = {enum_value};"
            lines.append(
                f'{indent}if (compare(arg, {cpp_string(choice)})) '
                f'{{ {body} matched = true; }}'
            )
        lines.append(
            f'{indent}if (!matched) {{ std::fprintf(stderr, "invalid enum for '
            f'{_context_human(target_expr)}.{arg.name}: %s\\n", arg); return 2; }}'
        )
    else:
        raise ValueError(f"unsupported arg kind: {arg.kind}")
    return lines


def _context_human(target_expr: str) -> str:
    parts = [part for part in target_expr.split(".") if part not in {"options"}]
    return ".".join(parts) if parts else "global"


def emit_arg_consume(
    args: list[Arg],
    target_expr: str,
    count_var: str,
    context_label: str,
    indent: str,
) -> list[str]:
    if not args:
        return [
            f'{indent}std::fprintf(stderr, "unexpected argument for '
            f'{context_label}: %s\\n", arg);',
            f"{indent}return 2;",
        ]

    fixed = [arg for arg in args if not arg.variadic]
    variadic = args[-1] if args[-1].variadic else None
    lines: list[str] = []
    for index, arg in enumerate(fixed):
        keyword = "if" if index == 0 else "else if"
        lines.append(f"{indent}{keyword} ({count_var} == {index}) {{")
        body = emit_arg_assignment(arg, target_expr, f"{indent}    ")
        body.append(f"{indent}    {count_var} += 1;")
        lines.extend(body)
        lines.append(f"{indent}}}")
    if variadic is not None and fixed:
        lines.append(f"{indent}else {{")
        lines.extend(
            emit_arg_assignment(variadic, target_expr, f"{indent}    ")
        )
        lines.append(f"{indent}}}")
    elif variadic is None:
        lines.append(f"{indent}else {{")
        lines.append(
            f'{indent}    std::fprintf(stderr, "too many arguments for '
            f'{context_label}: %s\\n", arg);'
        )
        lines.append(f"{indent}    return 2;")
        lines.append(f"{indent}}}")
    else:
        lines.extend(emit_arg_assignment(variadic, target_expr, f"{indent}"))
    return lines


def emit_required_validation(
    args: list[Arg],
    count_var: str,
    context_label: str,
    indent: str,
) -> list[str]:
    lines: list[str] = []
    required_count = sum(1 for arg in args if not arg.variadic and not arg.has_default)
    if required_count == 0:
        return lines
    lines.append(f"{indent}if ({count_var} < {required_count}) {{")
    lines.append(
        f'{indent}    std::fprintf(stderr, "missing required argument for '
        f'{context_label}\\n");'
    )
    lines.append(f"{indent}    return 2;")
    lines.append(f"{indent}}}")
    return lines


def make_cli_source(
    flags: list[Flag],
    args: list[Arg],
    commands: dict[str, Command],
) -> str:
    all_flags = list(flags)
    for command in commands.values():
        all_flags.extend(command.flags)
        for subcommand in command.subcommands.values():
            all_flags.extend(subcommand.flags)
    uses_append_split = any(flag.kind == "list" for flag in all_flags)
    reset_uses_strings = has_string_fields(flags, args)
    for command in commands.values():
        reset_uses_strings = reset_uses_strings or has_string_fields(
            command.flags, command.args
        )
        for subcommand in command.subcommands.values():
            reset_uses_strings = reset_uses_strings or has_string_fields(
                subcommand.flags, subcommand.args
            )
    if uses_append_split:
        append_split_block = [
            "static void appendSplit(zith::memory::DynArray<InternId> &out, "
            "zith::memory::StringInterner &strings,",
            "                        std::string_view value, char separator) {",
            "    std::size_t start = 0;",
            "    while (start <= value.size()) {",
            "        const std::size_t at = value.find(separator, start);",
            "        std::string_view item = value.substr(",
            "            start, at == std::string_view::npos ? std::string_view::npos : at - start);",
            "        if (!item.empty())",
            "            out.push(strings.intern(item));",
            "        if (at == std::string_view::npos)",
            "            break;",
            "        start = at + 1;",
            "    }",
            "}",
        ]
    else:
        append_split_block = []

    lines = [
        '#include "cli.hpp"',
        '#include "actions.hpp"',
        "",
        "#include <cstdio>",
        "#include <cstdlib>",
        "#include <cstring>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace generated_cli {",
        "",
        "static bool compare(const char *a, const char *b) {",
        "    return std::strcmp(a, b) == 0;",
        "}",
        "",
        "template <class... Args> static bool compare(const char *a, Args &&...args) {",
        "    return (compare(a, std::forward<Args>(args)) || ...);",
        "}",
        "",
        "static bool parseLong(const char *text, long &value) {",
        "    char *end = nullptr;",
        "    value = std::strtol(text, &end, 10);",
        "    return end != nullptr && *end == '\\0';",
        "}",
        "",
        *append_split_block,
        "",
    ]
    lines.append("static void resetToDefaults(Options &options, "
                 "zith::memory::StringInterner &strings) {")
    lines.extend(reset_statements(flags, args, commands))
    lines.append("    (void)strings;")
    lines.append("}")
    lines.append("")
    lines.append("int Cli::parseArgs(int argc, char **argv) {")
    lines.append("    command = Command::None;")
    if args:
        lines.append("    std::size_t globalArgCount = 0;")
    for command in commands.values():
        if command.args:
            lines.append(f"    std::size_t {command.member_name}ArgCount = 0;")
        if command.subcommands:
            lines.append(f"    {command.subcommand_member_name} = "
                         f"{command.subcommand_enum_name}::None;")
        for subcommand in command.subcommands.values():
            if subcommand.args:
                lines.append(
                    f"    std::size_t {command.member_name}"
                    f"{to_pascal(subcommand.name)}ArgCount = 0;"
                )
    lines.append("    resetToDefaults(options, strings);")
    lines.append("    for (int i = 1; i < argc; ++i) {")
    lines.append("        const char *arg = argv[i];")
    lines.append("")

    action_flags = [flag for flag in flags if flag.kind == "action"]
    ordinary_global_flags = [flag for flag in flags if flag.kind != "action"]
    for flag in action_flags:
        lines.append(emit_flag_handler(flag, "options", "        "))
    if action_flags:
        lines.append("")

    for command in commands.values():
        if not command.subcommands:
            continue
        for subcommand in command.subcommands.values():
            if not subcommand.flags:
                continue
            lines.append(
                f"        if (command == Command::{command.enum_name} && "
                f"{command.subcommand_member_name} == "
                f"{command.subcommand_enum_name}::{subcommand.enum_name}) {{"
            )
            for flag in subcommand.flags:
                lines.append(
                    emit_flag_handler(
                        flag,
                        f"options.{command.member_name}.{subcommand.member_name}",
                        "            ",
                    )
                )
            lines.append("        }")
            lines.append("")

    for command in commands.values():
        if not command.flags:
            continue
        lines.append(f"        if (command == Command::{command.enum_name}) {{")
        for flag in command.flags:
            lines.append(emit_flag_handler(flag, f"options.{command.member_name}", "            "))
        lines.append("        }")
        lines.append("")

    for flag in ordinary_global_flags:
        lines.append(emit_flag_handler(flag, "options", "        "))
    if ordinary_global_flags:
        lines.append("")

    # Posicionais globais: consumidos apenas antes de um comando ser selecionado.
    lines.append("        if (command == Command::None) {")
    for command in commands.values():
        lines.append(
            f"            if (compare(arg, {cpp_string(command.name)})) {{ "
            f"command = Command::{command.enum_name}; continue; }}"
        )
    lines.extend(
        emit_arg_consume(
            args,
            "options",
            "globalArgCount",
            "global",
            "            ",
        )
    )
    lines.extend(
        [
            "            continue;",
            "        }",
            "",
        ]
    )

    lines.extend(
        [
            "        if (arg[0] == '-') {",
            '            std::fprintf(stderr, "unknown flag: %s\\n", arg);',
            "            return 2;",
            "        }",
        ]
    )

    for command in commands.values():
        if command.args and not command.subcommands:
            lines.append(f"        if (command == Command::{command.enum_name}) {{")
            lines.extend(
                emit_arg_consume(
                    command.args,
                    f"options.{command.member_name}",
                    f"{command.member_name}ArgCount",
                    command.name,
                    "            ",
                )
            )
            lines.extend(
                [
                    "            continue;",
                    "        }",
                    "",
                ]
            )

    for command in commands.values():
        if not command.subcommands:
            continue
        lines.append(
            f"        if (command == Command::{command.enum_name} && "
            f"{command.subcommand_member_name} == {command.subcommand_enum_name}::None) {{"
        )
        for subcommand in command.subcommands.values():
            lines.append(
                f"            if (compare(arg, {cpp_string(subcommand.name)})) {{ "
                f"{command.subcommand_member_name} = "
                f"{command.subcommand_enum_name}::{subcommand.enum_name}; continue; }}"
            )
        lines.append(
            f'            std::fprintf(stderr, "unknown subcommand for {command.name}: %s\\n", arg);'
        )
        lines.append("            return 2;")
        lines.append("        }")
        lines.append("")

        for subcommand in command.subcommands.values():
            if not subcommand.args:
                continue
            lines.append(
                f"        if (command == Command::{command.enum_name} && "
                f"{command.subcommand_member_name} == "
                f"{command.subcommand_enum_name}::{subcommand.enum_name}) {{"
            )
            lines.extend(
                emit_arg_consume(
                    subcommand.args,
                    f"options.{command.member_name}.{subcommand.member_name}",
                    f"{command.member_name}{to_pascal(subcommand.name)}ArgCount",
                    f"{command.name}:{subcommand.name}",
                    "            ",
                )
            )
            lines.extend(
                [
                    "            continue;",
                    "        }",
                    "",
                ]
            )

    lines.append("        return 2;")
    lines.append("    }")
    lines.append("")
    if args:
        lines.extend(emit_required_validation(args, "globalArgCount", "global", "    "))
    for command in commands.values():
        if command.args and not command.subcommands:
            lines.append(f"    if (command == Command::{command.enum_name}) {{")
            lines.extend(
                emit_required_validation(
                    command.args,
                    f"{command.member_name}ArgCount",
                    command.name,
                    "        ",
                )
            )
            lines.append("    }")
        for subcommand in command.subcommands.values():
            if subcommand.args:
                lines.append(
                    f"    if (command == Command::{command.enum_name} && "
                    f"{command.subcommand_member_name} == "
                    f"{command.subcommand_enum_name}::{subcommand.enum_name}) {{"
                )
                lines.extend(
                    emit_required_validation(
                        subcommand.args,
                        f"{command.member_name}{to_pascal(subcommand.name)}ArgCount",
                        f"{command.name}:{subcommand.name}",
                        "        ",
                    )
                )
                lines.append("    }")
    lines.append("    return 0;")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace generated_cli")
    return "\n".join(lines) + "\n"


def make_gitignore() -> str:
    return "cli.hpp\ncli.cpp\nactions.hpp\n"


def write_generated(
    out_dir: Path,
    flags: list[Flag],
    args: list[Arg],
    commands: dict[str, Command],
    actions: list[Action],
) -> None:
    (out_dir / "cli.hpp").write_text(
        make_cli_header(flags, commands, args), encoding="utf-8"
    )
    (out_dir / "cli.cpp").write_text(
        make_cli_source(flags, args, commands), encoding="utf-8"
    )
    (out_dir / "actions.hpp").write_text(make_actions_header(actions), encoding="utf-8")
    (out_dir / ".gitignore").write_text(make_gitignore(), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/cli/cli.rules")
    parser.add_argument("--out", default="src/cli", help="diretorio de saida")
    args = parser.parse_args()

    rules_path = Path(args.rules)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2

    try:
        flags, args, commands, actions = parse_rules(
            rules_path.read_text(encoding="utf-8"),
            rules_path,
        )
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    out_path.mkdir(parents=True, exist_ok=True)
    write_generated(out_path, flags, args, commands, actions)
    print(f"generated {out_path / 'cli.hpp'}")
    print(f"generated {out_path / 'cli.cpp'}")
    print(f"generated {out_path / 'actions.hpp'}")
    print(f"generated {out_path / '.gitignore'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
