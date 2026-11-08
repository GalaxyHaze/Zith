# Full-Zith Annotation Order and the `@honest` Rename

Status: accepted as a full-Zith design direction. It does not change Zith--.

Full Zith fixes one canonical order for the prefixes on a callable
declaration, and renames the middle purity classification from `@valuePure` to
`@honest`. Both were left open by
[ADR-0039](0039-full-zith-capability-and-purity-contracts.md). The order
spans three dimensions, so it is recorded here rather than inside the safety
decision in
[ADR-0043](0043-full-zith-safety-levels-safe-trust-hazard.md).

## Canonical annotation order

A callable declaration reads its prefixes from the widest scope to the
narrowest:

```zith
#[volatile] @using(Fs) @honest trust pub extern fn read(path: string): string
```

| Position | Prefix | Dimension |
|---|---|---|
| 1 | `#[...]` | Mechanism attributes |
| 2 | `@using(...)` / `@only(...)` | Capability-use contract |
| 3 | `@pure` / `@honest` / `@impure` | Purity classification |
| 4 | `safe` / `trust` / `hazard` | Safety level |
| 5 | `pub` / `mod` | Visibility |
| 6 | `fn` / `extern fn` / `state` | Function kind |

Mechanism attributes come first because they describe what the compiler does
with the declaration. Capability use follows because it constrains the body.
Purity and safety are both classifications of the callable and sit between the
contracts and the visibility. Visibility and kind close the declaration.

Safety is orthogonal to the function kind, so a combination such as
`trust extern fn` is legal and reads as an audited C binding.

The formatter accepts any order on input and rewrites it to the canonical
order. There is one legal spelling in formatted source.

The same order applies to a callable type, with the kind last. A callback type
carries its level and its purity in the same relative order, for example
`@honest safe fn(i32): i32`.

## The `@honest` rename

The three purity classifications form one ladder of strength over the returned
value, not a positive and its negation:

| Annotation | Requirement | Side effects |
|---|---|---|
| `@pure` | The value depends only on the explicit arguments, and the function is deterministic. | Not permitted. |
| `@honest` | The value depends only on the explicit arguments. | Permitted when they do not affect the value. |
| `@impure` | The value may depend on operations outside the explicit arguments. | Permitted. |

The ladder is total and the endpoints are exclusive. A `@pure` function also
satisfies `@honest`, because forbidding side effects is the stronger promise. A
value that depends on an impure operation cannot be `@honest`, because its
value is no longer a function of the arguments alone.

`@honest` replaces `@valuePure`. The old spelling named a weaker `@pure`, which
read as a single concept with a qualifier. The new spelling names what the
guarantee actually is, that the returned value does not lie about where it
comes from. It belongs to the existing plain-intention family of `@assume`,
`@ensure`, `@maybe`, and `@ok`.

`@impure` is retained. It is the bottom rung of the ladder, not a redundant
negative, and it is a pin like `@pure`. A function whose impurity is
load-bearing fails the build if it is later made honest by accident.

## Impure has two senses

Common usage calls a function impure when it has side effects. In this contract
the classification is about the value, not the effects. The two senses
separate:

```zith
fn log(msg: string): i32 { print(msg); return 0; }   // side effect, value independent of it -> @honest
fn counter(): i32 { global += 1; return global; }     // value depends on hidden state -> @impure
```

`log` has a side effect and is still `@honest`. `counter` is `@impure`
because its value is not a function of its arguments. The glossary records the
value sense as the contract meaning.

## Effect on Zith--

Zith-- does not implement the `@` annotation surface. It keeps its `#[...]`
attributes and none of `@using`, `@only`, `@pure`, `@honest`, or `@impure`.
The canonical order and the rename apply to Zith only, and this decision does
not change `Zith--`.
