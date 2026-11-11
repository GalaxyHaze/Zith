# C++ Interop Drawing

Status: drawing. The idea is recorded so a later reader does not reopen the
decision without the context, and so the C++ work is not started before the C
ABI surface is complete and verified.

This is the `drawing` state of the ABI lifecycle in
`/home/diogo/Zith/docs/specs/abi-lifecycle.md`. It is not a promise, not a
plan, and not signed. It is deliberately not an ADR: the decision fails the
ADR "hard to reverse" test because nothing is being built, so the cost of
changing this direction later is a document edit.

## Problem

Calling a C++ library from Zith would be convenient. C++ interop is large
enough that it must be scoped before it is promised, and the scope that makes
the idea tractable is much smaller than "the Itanium C++ ABI".

## Size

System. The slice would cross the frontend (linkage syntax), the C++ binder,
type layout, name mangling, code generation, and the linker. That is the same
surface as the C ABI work, which is why it cannot start before the C ABI
surface is complete and verified.

## Non-goals

- vtables, dynamic dispatch, or `this`-adjusting thunks.
- RTTI, `type_info`, `dynamic_cast`, or `typeid`.
- exceptions and unwinding.
- templates, and therefore template instantiation and COMDAT merging.
- operator overloading, namespaces beyond symbol qualification, and
  constructors/destructors.
- C++ standard library types (`std::string`, `std::vector`, and similar).
- A C++ frontend. Zith does not parse C++ source. It binds to C++ symbols.

## The slice

The only defensible scope, if the idea is ever promoted:

1. Itanium name mangling, so Zith can name an existing C++ symbol.
2. Standard-layout class ABI, so a C++ object that is compatible with C can
   be passed by value and by pointer.

Everything a C++ object needs beyond that, which is vtables, RTTI, exceptions,
and templates, is out of scope. The target ABI is the Itanium C++ ABI used by
the validated C targets (x86-64 and AArch64 Linux).

## Trigger

Promote the idea out of the drawing only when both hold. First, a concrete C++
library needs to be called, named in the promotion work. Second, the C ABI
surface is complete and verified against the C lifecycle seam.

Without a named library the slice has no consumer and no way to check the
mangling and layout against a real symbol.

## Risks

- The Itanium ABI is not only name mangling. Class layout, vtable layout, RTTI,
  and unwinding are separate concerns, and the slice must hold the line at
  mangling plus standard-layout classes.
- The current repository direction is the opposite: a Zith-owned C binder and a
  tiny C backend, away from libclang and LLVM. A C++ slice that depends on
  libclang would run against that direction. See ADR-0008.
- Standard-layout class ABI already overlaps the validated C record ABI. If
  that surface is complete, the C++ slice is mostly mangling plus a small
  layout extension, not a new subsystem.

## Related

- ADR-0008: Zith-owned C binder and tiny backend roadmap.
- ADR-0009: validated C ABI surface.
- `/home/diogo/Zith/docs/plans/abi/explicit-alignment.md`: the shape a signed
  ABI contract takes in this repository.
- `/home/diogo/Zith/docs/specs/abi-lifecycle.md`: drawing, promising, signing,
  conforming.
