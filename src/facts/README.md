# Zith facts v2

`facts` is a small constraint/state store for compiler logic and tooling. The v2
surface is a header-only API (`fact-v2.hpp` plus `fact-v2.tpp`) and does not use
generators. It replaces the earlier v1 `FactBuilder`.

## Model

`FactStore<T>` stores a persistent DAG of worlds: a single root, split children,
and merge nodes. A node owns only the facts and hard constraints introduced
there. A merge references its alternatives; it does not copy or intersect their
constraints.

`assume(world, formula, expected)` lowers boolean structure into hard worlds:

| Formula | expected | result |
| --- | --- | --- |
| `A and B` | true | assume `A`, then `B`, in one world |
| `A or B` | false | assume `not A`, then `not B`, in one world |
| `A or B` | true | one split child for `A`, one for `B` |
| `A and B` | false | one split child for `not A`, one for `not B` |

Each satisfiable path is solved independently. `Query` is joined only at a merge:

| Results in alternatives | Query |
| --- | --- |
| all true | `True` |
| all false | `False` |
| all unknown | `Unknown` |
| any mixture | `Maybe` |

`Unknown` means no path proves or disproves the relation. `Maybe` means paths give
different answers. Contradictions are separated from `Maybe`: the contradictory
path is discarded and recorded in `conflicts()`, and is never converted into
`Maybe`.

## World and fact semantics

- facts created on the root are visible everywhere;
- a fact created in a split child is visible only in that child and its
  descendants;
- a fact created after a merge is visible in that merge and in later children;
- a branch-local fact used in a constraint after a merge is a conflict, not a
  `Maybe` answer.

## Values and affine expressions

An atom is built from a fact, a constant, or `add(fact, offset)`. Thus
`equal(x, add(y, 4))` expresses `x == y + 4`; exact values propagate in both
directions and are independent of assumption order:

```cpp
store.assume(world, store.equal(y, store.constant(5)), true);
store.assume(world, store.equal(x, store.add(y, 4)), true);
store.domain(world, x); // exact 9
```

Overflow while applying an offset is a conflict, never a wrapped value:

```cpp
store.assume(world, store.equal(a, store.add(b, 1)), true);
store.assume(world, store.equal(b, store.constant(INT_MAX)), true); // conflict
```

`ValueDomain<T>` offers `unknown`, `exactValue`, and manual ranges with inclusive
or open endpoints. It can answer whether it contains a concrete value or another
exact domain.

## Query API

- `status(world, formula)` and `status(world, lhs, relation, rhs)` return
  `True`, `False`, `Unknown`, or `Maybe`;
- `domain(world, fact)` returns the exact/ranged value known in a single-path
  world, or an empty domain for a merged world;
- `visible(world, fact)` checks fact visibility;
- `conflicts()` and `hasConflicts()` report rejected paths for diagnostics.

## Demo and tests

- `tests/facts/facts-v2-demo.cpp` is a small runnable example that prints
  `facts-v2-demo: all examples passed`.
- `tests/facts/facts-v2-test.cpp` is the deterministic suite:
  - `facts-v2-test --basic` runs the semantic matrix for domains, relations,
    lowering, joins, visibility, affine propagation, and conflict isolation;
  - `facts-v2-test --intensive` additionally runs 256 reproducible RNG scenarios
    with seed `0xFACADE42`.

## Future `facts.rules` contract

The intended generator surface is a declarative `.rules` file. It is not yet
implemented; changing `tools/rules_kit/`, a `generate.py`, or any protected
subsystem remains out of scope.

A future file should be composed of domains, attributes, and implication rules:

```text
[domain] Version
  attribute stable : Bool
  attribute major  : Int

[rule]
  release & stable => publish

[rule]
  major >= 2 => stable
```

The generator would turn declarations into table-driven accessors, relation
wiring, and generated C++; behavior such as conflict diagnostics and merging
stays in handwritten code.
