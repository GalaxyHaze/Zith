# Zith Compiler

Zith is a statically typed C++23 compiler targeting a minimal systems-language subset called `Zith--`. The modern pipeline is the canonical implementation surface; legacy parser/sema artifacts are archived outside the active tree.

## Language

**Module**:
A `.zith` source unit that owns a set of public and module-local symbols and can be imported by other units.
_Avoid_: File, script, package

**Import**:
Load another module and bind it under a namespace or explicit alias without injecting its symbols into the current scope.
_Avoid_: include

**From-import**:
Load another module and inject its public symbols directly into the current module scope.
_Avoid_: wildcard import, using namespace

**Export**:
Make a dependency visible to consumers of the current module, both as its full path namespace and as injected public symbols.
_Avoid_: re-export alias, pass-through

**Module alias**:
The local name bound by `import Path as Name` that consumers use as the base of qualified access.
_Avoid_: namespace, shorthand

**Qualified name**:
A dot-separated route from an import alias or full module path to a public symbol.
_Avoid_: path access, dotted name

**Public symbol**:
A declaration visible to importing modules, written with `pub`.
_Avoid_: exported symbol, visible symbol

**Module path**:
The slash-separated location of a module relative to a visible import root, written with `/`, not `.`.
_Avoid_: package path, namespace path

**C macro constant**:
A scalar object-like `#define` imported from a C header and exposed to Zith as an immutable foreign constant.
_Avoid_: macro, define constant

## Architecture

**Frontend context**:
Owns module loading, import graph construction, public symbol merging, and per-module name resolution for the modern pipeline.
_Avoid_: importer, resolver, module loader

**Module resolution**:
The per-module mapping from expression nodes and bindings to concrete declarations, imports, and foreign C header entries.
_Avoid_: symbol table, binding table

## Standard Library

**Formatable**:
The trait implemented by values that can be rendered through `print`/`println`.
_Avoid_: Printable, displayable, serializer

**FormatBuffer**:
The backing object supplied to `Formatable.format(self, dest)` so a value can append its rendered text.
_Avoid_: string builder, write buffer, output stream

**InputLine**:
The wrapper returned by `input()`. It owns the read buffer, exposes the trimmed line through methods, and supports `cast<T>`.
_Avoid_: input string, result line, readline result

**ParseInput**:
The trait implemented by primitive input types that can be parsed from an `InputLine`; a failed parse returns `null`.
_Avoid_: parser, conversion trait, strconv

**IoError**:
The result type used by `print`/`println` (and potentially `input`) for allocation and write failures in Zith--.
_Avoid_: error union, result error, failable error

**Consume**:
The ownership convention for resource cleanup: `destroy(self: lend)` owns and consumes the value, calling `free`/release internally.
_Avoid_: destructor, drop, free method

**Primitive erasure**:
Erasing a primitive value to a `dyn Trait` fat pointer so it can be handled through the same dynamic-dispatch surface as structs.
_Avoid_: boxing, wrapping, vtbl primitive
