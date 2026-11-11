# C++ Interop Is Parked

Recorded after a design session (2026-11-08) that asked whether adding Itanium
ABI / C++ interop was worth it. The answer is no for now. The full reasoning is
in `docs/plans/abi/cpp-interop-drawing.md`. This note records the decision and
the non-obvious facts behind it.

## Decision

- C++ interop is parked, not planned.
- The only defensible slice, if it is ever promoted, is Itanium name mangling
  plus standard-layout class ABI, with no vtables, RTTI, exceptions, or
  templates. The glossary term is `C++ interop slice` in `CONTEXT.md`.
- Promotion needs both a named C++ library to call and a completed, verified C
  ABI surface.
- No ADR: nothing is built, so the decision is cheap to reverse and fails the
  ADR "hard to reverse" test.

## Non-obvious facts

- "Itanium ABI" is mostly not name mangling. Mangling is a small part. Class
  layout, vtable layout, RTTI, unwinding, guard variables, and thunks are the
  rest, and each is a separate subsystem.
- Zith emits no C++ mangling today. Non-extern functions get a bespoke
  human-readable linkage name `<module>.<Owner>.<name>(<types>)` from
  `src/sema/hir-lower-modern.cpp`. Only `extern fn`, `main`, and `= extern
  CSymbol` produce C-ABI symbols. There is no `_Z...` grammar anywhere.
- The repository direction is a Zith-owned C binder and a tiny C backend, away
  from libclang and LLVM (ADR-0008). C++ interop via libclang would run against
  that direction.

## Where the C ABI work goes instead

The session also confirmed the C ABI is the priority. The concrete C ABI gaps
found are recorded in `docs/18-c-interop.md` and the impl-status table. The two
most severe are reproduced in `memory/cinterop-foreign-enum-broken.md`.
