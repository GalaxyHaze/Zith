# Zith Concept Registry

Machine-checked by `scripts/check-concepts.py`. One row per concept. See
[README.md](README.md) for the column contract and the check's rules.

Authority is `path#anchor` where `anchor` is the GitHub heading slug. Status is
`prose` (a chapter section defines it), `appendix` (only the spec appendix
table defines it), or `adr` (only an ADR decides it).

## Compiler Intrinsics (spec appendix A.2)

| Concept | Spellings | Authority | Decision | Sibling | Status |
|---|---|---|---|---|---|
| Field iteration | `@fields` | docs/11-comptime.md#113-reflection | - | - | prose |
| Type size | `@sizeOf` | docs/Zith-spec.md#a2-compiler-intrinsics | - | docs/mra-spec.md | appendix |
| Trait membership test | `@hasTrait` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Type-kind predicate | `@struct`, `@component`, `@union`, `@enum` | docs/11-comptime.md#113-reflection | - | - | prose |
| Nullable predicate | `@nullable` | docs/11-comptime.md#113-reflection | - | - | prose |
| Primitive predicate | `@primitive` | docs/11-comptime.md#113-reflection | - | - | prose |
| Region allocation | `@allocate` | docs/Zith-spec.md#a2-compiler-intrinsics | - | docs/mra-spec.md | appendix |
| Capture pack | `@pack` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Pack to struct | `@toStruct` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Struct to pack | `@toPack` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Append field | `@appendField` | docs/11-comptime.md#114-type-manipulation | - | - | prose |
| Remove field | `@removeField` | docs/11-comptime.md#114-type-manipulation | - | - | prose |
| Append method | `@appendMethod` | docs/11-comptime.md#114-type-manipulation | - | - | prose |
| Source location | `@file`, `@line`, `@fnName` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Rich location | `@location` | docs/Zith-spec.md#a2-compiler-intrinsics | - | - | appendix |
| Failable validity test | `@ok` | docs/08-error-handling.md#81-failable-types | docs/adr/0030-failable-invalid-state-model.md | - | prose |
