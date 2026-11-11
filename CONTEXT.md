# Zith Languages

This context defines the relationship between the sibling languages Zith and Zith-- and the terms used in their compiler repository.

## Language

**Zith**:
The systems language specified by `docs/Zith-spec.md`. It is distinct from and a sibling language to `Zith--`.
_Avoid_: describing Zith as a superset of Zith--

**Full Zith**:
A label used in design documents for Zith when they contrast it with `Zith--`. The label does not imply a superset relationship.
_Avoid_: full-Zith subset

**Zith--**:
A distinct systems language with its own specification and compiler status. Its implementation status does not describe Zith.
_Avoid_: subset of Zith, Zith dialect

**Zith-- compiler status**:
The implementation status of the compiler against the `Zith--` language contract. It says nothing about the implementation status of Zith.
_Avoid_: Zith compiler maturity

**Universal Zith API**:
The shared public-API design style intended to remain familiar across Zith implementation styles. It guides public API shape and is distinct from a project's implementation policy.
_Avoid_: universal subset, project identity

**Project language identity**:
A project-local policy that enables or disables optional features in project-owned code and can explain those restrictions in diagnostics. It does not redefine the universal API style or apply to dependencies.
_Avoid_: language dialect, dependency profile

**Zith context**:
A specialized syntax integration mechanism that a domain-facing API may offer for domains such as Math, SQL, or HTML. It is not an ordinary public API surface or a general-purpose API abstraction.
_Avoid_: public API container, namespace

**Zith tag**:
A full-Zith domain-syntax construct, distinct from macros and compiler intrinsics. Its invocation is delimited by `@<` and `</`.
_Avoid_: tag macro, intrinsic tag

**Tag body kind**:
The representation contract for a Zith tag's required body. Established kinds are `tokens` for exact source text, `identifier` for one identifier, `ast` for structured syntax, and `block` for Zith code.
_Avoid_: macro argument kind, body meta-type

**Zith-- macro**:
A source-level `macro` declaration in Zith--. It defines a reusable syntax-template expansion that runs during frontend lowering, before semantic analysis.
_Avoid_: C macro, comptime function, inline function

**Zith-- macro argument kind**:
The declared contract for a Zith-- macro parameter: `val`, `identifier`, or `ast`.
_Avoid_: parameter meta-type, metatype, expr parameter, body parameter

**Variadic macro parameter**:
A final macro parameter that binds an ordered tail of zero or more call-site arguments.
_Avoid_: macro slice, argument spread

**Type-kind predicate**:
A compile-time test of whether a type is a `struct`, `component`, `enum`, or `union`, without inspecting or changing its structure.
_Avoid_: field reflection, type mutation

**Macro statement repetition**:
A template operation that emits one statement for each argument bound to a variadic macro parameter, preserving argument order.
_Avoid_: runtime loop, function variadic

**Zith-- macro value argument**:
An evaluated argument produced by normal call-site expression semantics, without explicit unevaluated syntax.
_Avoid_: expression parameter, value expression, evaluated expression

**Zith-- macro identifier argument**:
An argument bound from a single unqualified `Name` token. Qualified names, fields, and imported members are not identifier arguments.
_Avoid_: name parameter, identifier expression, simple name argument

**Zith-- macro AST argument**:
An unevaluated syntactic expression requested at the call site with `=expr`, where `expr` may include a block literal.
_Avoid_: pass-by-AST, quoted expression, raw expression, unevaluated expression

**Zith-- macro call attribute**:
A named call-site attribute written in the `|name: value|` position and made visible through the macro body as `attributes.name`.
_Avoid_: annotation, tag attribute, macro attribute list

**Zith-- explicit alignment**:
An explicit request to strengthen a struct type's alignment or one binding's
storage alignment beyond its natural alignment.
_Avoid_: `@alignas`

**Module**:
A `.zith` source unit that owns a set of public and module-local symbols and can be imported by other units.
_Avoid_: File, script, package

**Zith package**:
A named, versioned distribution unit that provides one or more Zith modules.
_Avoid_: module, library namespace, std package

**Package cache**:
A reusable store of compiled package artifacts, separate from the dependencies
selected by a project.
_Avoid_: package installation, dependency selection

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

**Syntax continuation**:
A parse-time construct produced by contextual syntax that consumes a following
syntax form and resolves to an ordinary value or another continuation. It is
not a runtime value.
_Avoid_: returned token, runtime builder

**Enum**:
A closed set of named numeric constants in Zith--, distinct from a runtime
union. `enum: union` is not part of the Zith-- contract.
_Avoid_: tagged union

**Union**:
A runtime-tagged value that contains one of its declared member types, which may be
heterogeneous.
_Avoid_: enum

**Bare**:
A position that carries no access or lifetime qualifier, so the value in it can
be copied directly. A `union` member is bare, and a `const` value is bare. The
qualifier lives on the value that owns the position, not on the position.
_Avoid_: unqualified, plain, default

**Const value**:
A full-Zith compile-time value. It is bare by definition, carries no access or
lifetime qualifier and no `mut`, and is copyable directly. Compile-time
mutation is a `var` inside a `const { ... }` block or a `const fn` body.
_Avoid_: comptime binding, runtime constant

**Anonymous state**:
A `state` literal without a source-level name that produces a state value for a state machine.
_Avoid_: state closure

**Trait**:
A named nominal contract for behavior or capabilities that a type explicitly implements.
_Avoid_: interface

**Trait composition**:
A relationship that combines traits' method contracts, defaults, and capabilities without inheriting or embedding type fields.
_Avoid_: inheritance, embedding

**Structural contract**:
A full-Zith `contract` declaration whose conjunctive type and value conditions are satisfied structurally. It does not provide behavior or dynamic dispatch.
_Avoid_: full-Zith interface, dynamic contract

**Zith-- interface**:
A structural static contract in Zith--, satisfied automatically when its conditions hold.
_Avoid_: trait, dynamic interface

**Capability trait**:
A nominal trait that names an ability, such as `Fs`, for static capability-use contracts.
_Avoid_: runtime effect, purity classification

**Capability-use annotation**:
A full-Zith `@using` or `@only` requirement over nominal capability traits, independent of purity and the NRA effect header.
_Avoid_: purity annotation, NRA effect header

**Honest function**:
A full-Zith callable whose returned value depends only on its explicit
arguments, even when it also performs side effects that do not affect that
value. It is written `@honest`, sits between `@pure` and `@impure` on the
purity ladder, and replaces the former `@valuePure` spelling.
_Avoid_: value-pure function, side-effect-free function, deterministic function

**Impure function**:
A full-Zith callable whose returned value may depend on operations outside its
explicit arguments. It is written `@impure` and is the bottom rung of the
purity ladder. The contract sense is about the value, not about whether the
callable has side effects.
_Avoid_: effectful function, side-effecting function

**Raw operation**:
A full-Zith operation that bypasses a compiler check, split by blast radius. Light raw operations (`raw`) break only when misused, so their damage is contained by correct input. Heavy raw operations (`hazard`) break assumptions the compiler relies on, so they can corrupt or miscompile even when written correctly and their risk is not bounded by the operation itself.
_Avoid_: unsafe operation, raw fn, hazard operation

**Safety level**:
The static classification of a callable by the raw operations its reachable body uses and whether it seals them. It is inferred when the callable is unannotated and pinned when written.
_Avoid_: purity classification, capability requirement, trust level

**Safe function**:
A full-Zith callable whose body reaches no raw operation and opens no hazard block. It is written `safe` to pin the guarantee. A body that reaches a raw operation is a compile error.
_Avoid_: pure function, trusted function

**Trust function**:
A full-Zith callable that uses light raw operations directly and heavy ones only inside a hazard block, and whose call site is sealed to the safe level. It is written `trust`, is the audited boundary that vouches for the raw work it contains, and may mark proven raw-free regions with a safe block.
_Avoid_: unsafe function, sealed function

**Hazard function**:
A full-Zith callable that uses any raw operation and calls other hazard functions directly, and whose call site propagates the hazard level to its caller. It is written `hazard` and does not vouch for its own safety.
_Avoid_: untrust fn, unsafe fn, raw fn, trusted function

**Hazard block**:
A full-Zith block that grants access to heavy raw operations and to hazard calls inside an otherwise-restricted body, and seals that access locally. It is the only place a trust function may use heavy raw operations.
_Avoid_: unsafe block, raw block

**Safe block**:
A full-Zith block that revokes access to every raw operation and hazard call inside it. The compiler verifies the revocation, so a safe block is a proven raw-free region rather than an asserted one.
_Avoid_: unsafe-off block, pure block

## Architecture

**Frontend context**:
Owns module loading, import graph construction, public symbol merging, and per-module name resolution for the modern pipeline.
_Avoid_: importer, resolver, module loader

**Module resolution**:
The per-module mapping from expression nodes and bindings to concrete declarations, imports, and foreign C header entries.
_Avoid_: symbol table, binding table

**ALIN**:
The alignment between an idea's extracted actions and its plan header and
body, used when turning ideas into feature plans. Header scope and body
promises must move together.
_Avoid_: backlog notes, orphan tasks, extract-then-forget

## Debt And Curation

**Implementation debt**:
A Zith-- feature that is implemented incompletely, with a known limitation
that is not an intended design boundary. Recorded in
`docs/implementation-debt.md` as a real debt, never as a non-debt.
_Avoid_: technical debt, code debt, TODO list

**Codebase quality debt**:
A structural or hygiene gap in the active tree, its docs, or its coverage
that does not change language behavior. Examples are duplicated status text,
untested execution paths, and contracts that depend on external services.
_Avoid_: implementation debt, feature gap, refactor backlog

**Non-debt**:
A deliberate Zith-- design boundary that looks incomplete against the larger
Zith spec but must be kept as-is. Recorded beside the debt inventory so it is
not reopened as work.
_Avoid_: accepted limitation, deferred feature, out-of-scope bug

## Pattern Matching

**When case**:
A single alternative of a `when` expression. It starts with a parenthesized condition area, followed by an optional `~>` marker and a body; `~>` is deprecated and remains only as an explicit escape.
_Avoid_: case, arm, branch, when branch

**When pattern island**:
The first parenthesized segment of a when case, always interpreted against the `when` subject.
_Avoid_: pattern block, first guard, subject block

**When continuation island**:
Any parenthesized segment after a top-level `and`/`or`/`xor` in a when case. It behaves as an ordinary boolean condition, not as another subject pattern.
_Avoid_: secondary pattern, multi-pattern, following guard

**When case separator**:
The mandatory comma between when cases, omitted for the final case.
_Avoid_: arrow delimiter, comma terminator, case delimiter

## Forward Dispatch

**Forward dispatch**:
A call-site construct that selects an overload from the runtime tag of one
marked argument and calls it with that argument. In Zith-- the subject is a
tagged union or an `opaque` value.
_Avoid_: forward overload, forward deduction, tag dispatch

**Forward dispatch subject**:
The single argument that carries the dispatch marker. Its type is the tagged
union or `opaque` value the construct dispatches over.
_Avoid_: dispatch target, dispatch receiver

## Ranges, `Contains` e `Iterator`

**Range**:
A literal expression `lo..hi` producing an interval between two raw bounds.
The AST/HIR keep `lo` and `hi` unchanged and store `openAtLo`/`openAtHi`
instead of adjusting the bounds.
_Avoid_: slice range, pattern shorthand, adjusted interval

**Range Bound**:
One of the two endpoints carried by a `Range`. A bound is closed by default;
`>` before `..` opens the lower bound and `<` after `..` opens the upper bound.
_Avoid_: offset bound, low+1 bound, high-1 bound

**Dots token**:
The frontend token spelling `Dots`, emitted for both `..` and `...` and distinguished by lexeme. It covers ranges, pipe placeholders, relative imports, `mod(..)`, and variadic slices without special-casing two or three separate dot characters.
_Avoid_: dot punctuation, two-dot marker, three-dot token

**Contains**:
The duck-typed protocol resolved by `value in rhs`: `contains(self, value): bool`.
It is independent of iteration and accepts any RHS type with that method,
including literal ranges.
_Avoid_: membership operator overload, range checking method, iterator method

**Iterator**:
The loop protocol resolved by `for (x in iterable)`: `next(self): ?T`.
It is deliberately separate from `Contains`; literal integer ranges are lowered
directly with an implicit step of `1` and float ranges are rejected for loops.
_Avoid_: Contains, membership protocol, range iteration via contains

## Runtime And Backends

**IR v2**:
The typed register execution IR used by the portable VM. It represents
Zith-- programs in a linear, assembly-like form with typed registers, calls,
branches, and traps.
_Avoid_: LLVM IR, bytecode, typed assembly, execution IR v1

**IR v2 VM**:
The C++ runtime that executes `IR v2` modules over a linear guest memory.
It is separate from the signed execution IR v1/`src/interp` path and is the
no-LLVM/WASM CLI runtime for the host Hello World slice.
_Avoid_: VM runtime, bytecode VM, v2 VM, execution engine

**IR v2 function reference**:
The VM representation of a `fn` or `extern fn` value as a typed register
holding an index into the module function/extern table, instead of a raw
host or guest pointer.
_Avoid_: function pointer, C ABI pointer, code address

**VM FFI subset**:
The small `extern fn` handler surface implemented by the VM, currently
covering host output and memory allocation (`puts`, `putchar`, `malloc`,
`free`, scoped `snprintf`, `strlen`, `memcpy`).
_Avoid_: libc shim, C FFI, native FFI, foreign function table

**Portable runtime target**:
The goal that real Zith-- entry examples (`from std/io/console` and manual
`extern fn printf`) run through the VM with observable output, traps, and
exit code matching the native path, without full libc or WASM packaging.
_Avoid_: browser runtime, playground runtime, full executable target

**ABI lifecycle**:
The four-state contract model for externally visible surfaces: drawing,
promising, signing, and conforming. A plan is a drawing until promises are
computed, an ADR signs those promises, and tests prove them.
_Avoid_: formal process, design review, PDF documentation

**Interpreter HIR**:
The in-process executor that runs Zith programs from the typed HIR without lowering
to a native object/executable. It is the portable execution path when LLVM is not
available and the baseline for future bytecode or comptime execution.
_Avoid_: runtime VM, HIR evaluator, fallback executor

**Runtime FFI handler**:
The internal table mapping a linkage name of an `extern fn` to a Zith-owned host
implementation, replacing host libc in portable/browser execution.
_Avoid_: C FFI, libc shim, native handler table

**Thread fork**:
The full-Zith core syntax `backend fork Entry(args)` that creates a thread through a backend object such as `pThread` and returns the backend's concrete `Thread<T>` handle.
_Avoid_: Thread capability, spawn keyword, ForkHandle

**Thread merge**:
The core keyword `merge` consumes thread handles and collects their results.
`and` waits for all handles and returns a tuple. `or` waits for all handles and
returns a tagged union covering all possible result types for the first thread
to finish. If threads finish simultaneously, the leftmost handle wins.
_Avoid_: join, wait, release handle

**Resource revocation**:
The `revoke` keyword removes a child flow's access to a resource without
destroying it or terminating the child. `revoke x;` revokes access to `x` from
every unbounded thread in the statement's scope that holds revocable access to
it. `revoke h1;` revokes all revocable resources passed to the thread
represented by handle `h1`.
_Avoid_: invalidation, cancellation, destruction

**Thread spawn**:
The full-Zith context shorthand that creates a blueprint and launches it in one
step, returning a handle. It is stdlib/context sugar, not a compiler keyword.
_Avoid_: core spawn keyword, runtime.spawn method

**Thread library spawn**:
The Zith-- `soon/thread` `backend.spawn(entry)` that creates a `Job` blueprint
and does not launch anything. Launch happens separately at `job.unbounded(args)`.
_Avoid_: launcher, running thread

**Thread backend**:
The full-Zith runtime capability/object that implements the `ThreadBackend`
capability and produces concrete thread handles for `fork`/`merge`, for example
`pThread`.
_Avoid_: Branch capability, thread allocator, fork factory

**Thread library backend**:
The Zith-- `soon/thread` trait `ThreadBackend` and its default implementation
`Thread`. A backend is an explicit value that builds a `Job` blueprint and
launches it through `unbounded`. It is ordinary runtime code and carries no
compiler ownership rule.
_Avoid_: full-Zith `ThreadBackend` capability, global thread runtime

**Thread handle**:
An owned, single-consumer value satisfying the `Thread<T>` protocol. `merge` consumes it exactly once; backend concrete handles may expose extra methods such as `detach`.
_Avoid_: ForkHandle, task, future

**ThreadHandle**:
The Zith-- `soon/thread` mergeable handle `ThreadHandle<T>`. `merge` blocks the
calling thread, consumes the handle once, and returns the entry result. It is
the library counterpart of the full-Zith `Thread<T>` handle.
_Avoid_: full-Zith `Thread<T>`, future, join token

**Job blueprint (draft)**:
A reusable description of work, named `Job` in the Zith-- `soon/thread`
library. It holds the entry and does not contain launch arguments; the launch
site supplies those separately.
_Avoid_: thread handle, running job

**Monitoring handle (draft)**:
A value associated with a launched execution so code can track it. In the
current thread discussion, it is distinct from the blueprint and from
`ThreadHandle<T>` and is not consumed by `merge`.
_Avoid_: job blueprint, merge handle

**ThreadError**:
The Zith-- `soon/thread` error enum. It carries `Unsupported` for a backend that
cannot start a thread and `Revoked` for a failed `Revokable` access. A thread
creation error is separate from the entry's own result.
_Avoid_: panic, host runtime error, entry result

**Share**:
The Zith-- `soon/thread` runtime wrapper `Share<T>` that lets several threads
read or update one value through explicit methods. It is a library wrapper, not
a compiler-enforced sharing rule.
_Avoid_: full-Zith `share` keyword, `MultiShare`

**Revokable**:
The Zith-- `soon/thread` runtime wrapper `Revokable<T>`, a reference whose
access the owner may revoke. A failed access returns `Result<T, ThreadError>`
with the `Revoked` variant. It is a library wrapper, distinct from the
full-Zith revocable-access qualifier described below.
_Avoid_: full-Zith revocable type qualifier, nullable borrow

**Executor**:
The runtime object that owns a work queue and a worker pool and runs submitted
state machines step by step. It is not a single thread and not the `dock`
caller.
_Avoid_: worker, thread handle, runtime scheduler

**Executor worker**:
One thread in an executor's pool that runs a single step of a task. Different
steps of one task may run on different workers.
_Avoid_: executor, thread handle

**I/O task**:
A submitted state machine together with its context, tracked by a handle until
it reaches a terminal value or is canceled. It is distinct from a `Thread<T>`
handle.
_Avoid_: thread handle, future, job blueprint

**Task context**:
The value an executor owns across the steps of one task and lends exclusively
to a step while it runs. Every step of a task receives the same context type.
_Avoid_: capture, environment

**Playground runtime**:
The browser-facing WASM module that checks Zith source, prepares HIR, and
executes it through the VM v2 IR with the same language and standard-library
surface as the native portable runtime. `run` is an execution operation;
`build` prepares compiler artifacts for later cache reuse.
_Avoid_: separate VM module, compiler-only WASM, HIR interpreter playground

**Stdlib pack**:
A versioned distribution artifact generated from the canonical `stdlib/` tree
for a target such as the browser WASM playground. It carries the standard
library sources and compatibility metadata; it is not a second hand-maintained
standard library.
_Avoid_: embedded stdlib copy, browser-only stdlib, virtual stdlib fork

**WASM HIR flat**:
The versioned, stateless binary representation of the lowered `HirModule`
produced by `zith_emit_hir`. It is self-contained: strings, types, functions,
and expressions can be reconstructed without the original source or compiler
session. Its layout is intentionally compatible with a future artifact cache
so cached HIR can be reused without a new lowering pass.
_Avoid_: ZIRL blob, cache bytecode, source snapshot, textual HIR dump

**WASM execution ABI**:
The exported contract with status codes `0` ok, `1` compile fail, `2` invalid
parameter, `3` runtime trap, `4` out of memory, and `5` unsupported construct,
including the high-level `zith_run_source` operation that compiles and
executes. `zith_emit_hir` and `zith_execute_hir` remain separate operations
for later cache integration.
_Avoid_: compile-once-run-once ABI, playground status enum, compiler diagnostics

**Host runtime error**:
The separate runtime failure channel for program execution (extern missing, panic,
division by zero) that is distinct from compiler diagnostics and from the program exit
status. Reuses `host_write` for output, but does not reuse `zith_last_error` as the only
error surface.
_Avoid_: compiler error, child exit status, runtime diagnostic

**CallRange**:
The register-window calling convention in IR v2 where the caller places argument values in a
contiguous block of local registers `[start_reg .. start_reg + count - 1]`. The VM copies this
range directly into parameter registers `0..count - 1` of the callee frame upon `Op::CallRange`.
_Avoid_: fixed 2-arg call, stack-spilled call frame, vararg table

**VM v2 hybrid dispatch**:
The two-tier call resolution mechanism in the VM v2 lowering pass: calls to well-known standard
library routines (such as `print` and `println`) match fast VM intrinsics or extern handlers first;
unmatched routines fall back to compiling and executing the canonical Zith standard library code.
_Avoid_: hardcoded stdlib skip, pure intrinsic runtime, interpreter-only bypass

**Exact-length stdout stream**:
Standard output emission in the VM v2 runtime (via `write_stdout`) that writes an exact byte
length from guest memory to the host terminal, supporting arbitrary non-null-terminated string
slices without injecting newlines.
_Avoid_: null-terminated stdout, puts-only output, line-buffered terminal dump

## Standard Library

**Standard library namespace**:
The import root that groups distributable library modules. `std` is the stable surface, `soon` is experimental, and `c` is the raw C FFI layer.
_Avoid_: stdlib root, package namespace, core library

**Stable library module**:
A module under `std/` that promises backward compatibility and can only change through the ABI lifecycle.
_Avoid_: core module, stable API, guaranteed module

**Experimental library module**:
A module under `soon/` that users may import for prototyping; its API may change or disappear before promotion to `std`.
_Avoid_: unstable std, beta module, preview API

**C binding module**:
A module under `c/` that maps a foreign C surface to Zith declarations without promising Zith-owned semantics. It is the raw FFI layer, not a safe wrapper.
_Avoid_: C wrapper, libc module, binding header

**Library module**:
The unit of standard library delivery: a relocatable `.zith` module under `std/`, `soon/`, or `c/` that owns its public types, traits, and functions.
_Avoid_: library unit, std package, module package

**Library domain**:
A coherent cluster of library modules built on the same contract, such as `io`, `collections`, `alloc`, or `fs`. The domain owns shared cross-module terms; each module owns its implementation.
_Avoid_: module group, feature area, std package

**Library contract**:
The trait/API surface a library module promises to consumers, for example formatting, parsing, hashing, or allocation. A `std` library contract is stable; a `soon` contract is provisional.
_Avoid_: interface, capability set, behavior contract

**Random generator**:
A stateful value that produces a pseudorandom sequence, advancing that
sequence as it produces values.
_Avoid_: random device, global random source

**Random algorithm**:
A named rule a random generator uses to produce pseudorandom values. It is
distinct from the source used to initialize the generator.
_Avoid_: device, seed source

**Seed**:
An initial value used to determine a random generator's starting sequence. An
explicit seed supports repeatable output.
_Avoid_: entropy source, random state

**Random stream**:
The ordered pseudorandom sequence associated with a generator as it advances.
_Avoid_: global sequence

**Uniform distribution**:
A sampling contract that gives every value in a finite discrete range equal probability.
_Avoid_: random range

**Primitive trait**:
A stdlib contract implemented for primitives, `?T`, or `[]T`, such as `Hashable`, `ParseInput`, or `Formatable`, so built-in values participate in the same traits as structs.
_Avoid_: primitive interface, builtin capability, intrinsic trait

**Resource**:
A value that owns storage or another runtime handle and requires an explicit cleanup path. In the current stdlib that path is `destroy(self: lend Self)`; the target path is `InPlace.clean` through `new`/`delete` or `make`/`release`.
_Avoid_: owner-managed type, handle, object

**Resource owner**:
The current holder of a resource and its cleanup obligation. Passing a resource by `view` or `lend` does not transfer cleanup responsibility.
_Avoid_: caller, responsible scope, lifetime owner

**Consuming parameter**:
A by-value parameter that receives ownership of its argument and the obligation to clean or transfer it.
_Avoid_: borrowing parameter, retaining parameter

**Retained reference**:
A non-owning reference that a callee may keep after the call. The caller keeps ownership and must keep the referenced resource valid while the reference is used.
_Avoid_: ownership transfer, temporary borrow

**Cleanup path**:
The explicit consuming operation that releases a resource. The current default is `destroy`; the target convention is the `new`/`delete` and `make`/`release` pairs.
_Avoid_: destructor, drop, teardown method

**Cleanup obligation**:
The current owner's duty to run a cleanup path or transfer that duty with ownership.
_Avoid_: ownership flag, destructor slot, pending resource

**Holder**:
A binding that names a resource owner and gives its cleanup obligation a scope that an explicit cleanup path can reach.
_Avoid_: owner name, cleanup site, lifetime scope

**Ephemeral owner**:
An owner whose lifetime ends before a handle that retains a reference to it, such as an arena created for a single call.
_Avoid_: temporary allocator, stack resource, short-lived value

**Consuming pipeline stage**:
A `|>` stage that transfers the current value to its operation exactly once on every execution path, then continues with the stage result.
_Avoid_: observing stage, implicit pipeline injection

**Observing pipeline stage**:
An `also` stage that borrows the current value for read-only use and leaves the chain value unchanged. Its `it` may be unused or read more than once, but the borrow cannot escape the stage.
_Avoid_: consuming stage, chain transform

**Storage block**:
The untyped byte region returned by an `Allocator` primitive. A storage block becomes typed only when `InPlace` construction or a collection reserves it for a concrete layout.
_Avoid_: raw allocation, memory chunk, buffer memory

**Allocator**:
The stdlib trait for raw storage: `alloc`, `free`, and `realloc`. It knows size and alignment, not object layout or construction.
_Avoid_: memory manager, heap wrapper, object allocator

**InPlace**:
The stdlib construction/cleanup trait used with an allocator-provided block: `inplace` builds the object into reserved storage and `clean` releases object-owned resources without freeing the block. It deliberately does not name `Allocator`, so the memory module graph stays a DAG.
_Avoid_: allocator-aware constructor, placement new, lifecycle trait

**Collection**:
A library type that owns storage, stores values under a stable lookup or enumeration contract, and exposes explicit capacity, count, and mutation operations.
_Avoid_: container, data structure, buffer

**Uniform collection API**:
The repeated stdlib collection shape: owned storage, explicit `len`/`capacity`, `contains`, `get`, mutation with `lend`, and consuming cleanup. It keeps collection users on one ownership model.
_Avoid_: standard container interface, collection protocol, common container

**Erasure support**:
The stdlib requirement that values can flow through a `dyn Trait` fat pointer. `Primitive erasure` covers primitives; broader support decides whether types like `string` can join homogeneous variadic tails.
_Avoid_: runtime type support, reflection requirement, polymorphism support

**Optional dyn receiver**:
A `?dyn Trait` value used as a method-call receiver. It carries the same two-word fat pointer as `dyn Trait` and a null data pointer marks the `null` state, so dispatch is reachable only after an `is null` narrowing proves the payload present.
_Avoid_: nullable dyn, tagged dyn pointer

**Dynamic projection (`dyn T`)**:
A reference to a `T` that lives inside another value, together with the
`outerTypeId` of the container and the byte `offset` of the payload in it. It
is distinct from `dyn Trait`: `dyn Trait` erases the implementation and
dispatches through a vtable, while `dyn T` keeps the inner type `T` and erases
the container. A checked cast back to the container type rebuilds the
container from the `outerTypeId` and `offset`. It does not own its payload, so
it cannot take `own`. See [ADR-0056](docs/adr/0056-nra-erased-forms-and-spelling.md).
_Avoid_: dyn trait, type-erased value, opaque reference

**Failable**:
A Zith capability for values with valid and invalid states, distinct from ownership and from the kind of invalid state a value carries.
_Avoid_: result wrapper, optional wrapper

**Valid-state test (`@ok`)**:
The discriminator used with `is` to narrow a `Failable` value to its valid state; the alternative branch proves that the value is invalid.
_Avoid_: success extractor

**Invalid**:
A Zith capability that a `Failable` type may implement to expose methods callable only after flow analysis proves that its value is invalid.
_Avoid_: null-only capability, error-only capability

**Nil**:
The compiler-defined universal absence state. `Nil` is invalid, but it is not an error value.
_Avoid_: error, exception

**Error**:
A Zith marker capability implemented by the type of an invalid value to classify that value as an error. It is not a generic error type and does not define a failable value's state.
_Avoid_: invalid state, absence, generic error type

**Request-scoped allocation**:
The shipped allocation model where a raw block is tied to the request/function that allocated it and must be freed by the same owner with the same `size`/`align`. It deliberately avoids global heap discovery.
_Avoid_: function-local allocation, temporary allocation, one-shot buffer

**Boxed resource**:
A resource created through `new`/`delete` or `make`/`release`, so the block, allocator, and cleanup are explicit at the call site. It is distinct from stack resources cleaned directly with `destroy`.
_Avoid_: heap object, allocated value, box

**Formatable**:
The trait implemented by values that can be rendered through `print`/`println`.
_Avoid_: Printable, displayable, serializer

**FormatBuffer**:
The backing object supplied to `Formatable.format(self, dest)` so a value can append its rendered text.
_Avoid_: string builder, write buffer, output stream

**TextSink**:
The intended sink contract for formatting, with capacity, length, append and text lookups through a mutable dynamic receiver (`lend dyn TextSink`). Blocked in the current compiler, so `FormatBuffer` is the practical sink until mutable dyn receivers work.
_Avoid_: output stream, writer, string builder

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

## Testing

**Zith test function**:
A free function annotated `#[test]` that the `zithc test` runner discovers and executes as one test. It takes no parameters and needs no `main`.
_Avoid_: test case, test method, spec

**Test driver**:
The entry point synthesized by the compiler for a test module, which calls each discovered Zith test function in turn and reports per-test results.
_Avoid_: test main, harness entry, generated runner

**Test result table**:
The compiler-synthesized table with one slot per discovered Zith test function. A slot holds `0` when no fail-fast assertion ended the test, or `N` when it returned at its `N`th failure site. Accumulated `@check` failures are reported as separate events. The table is runner infrastructure, not a user-writable global.
_Avoid_: results array, error table, global accumulator

**Assertion intrinsic**:
The compiler primitive `@assert(cond)` that ends the current test when its condition is false. Outside a test it panics.
_Avoid_: assert macro, expect

**Check intrinsic**:
The compiler primitive `@check(cond)` that records a false condition and continues within a Zith test. Outside the lexical test body it panics. Standard-library check macros expand at the call site so they inherit this context.
_Avoid_: check function, soft assertion

**Test log intrinsic**:
The compiler primitive `@log(msg)` that appends `[]char` to the current test's log buffer. The driver prints a test's log only when it fails, or for every test under verbose output.
_Avoid_: print, debug print, test output

## Generics

**Reified generic type**:
A concrete `Name<A, B>` type produced from a generic declaration by substituting each generic parameter with a concrete type argument. It owns a substituted layout and a concrete type name.
_Avoid_: instantiated template, monomorphized symbol, concrete template type

**Concrete type argument**:
A concrete type bound to one generic parameter of a reified generic type. `StructType` stores these arguments for method resolution and cache round-trips.
_Avoid_: generic slot, field-inferred parameter, runtime type argument

**Generic bound source**:
The declaration-level parameter binding that owns a bound such as `K: Hashable`. It stays authoritative even when the bound is reached through a generic field.
_Avoid_: bound metadata on a concrete type, field-derived bound, receiver-derived trait

**Imported generic instantiation**:
The resolution path that reifies a generic declaration reached through a module alias, using the declaring module's template as the source of truth and producing the same reified type and method metadata as a local instantiation.
_Avoid_: cross-module monomorphization, qualified template lookup, imported template special case

## Zith Proof Kernel (ZPK)

**Zith Proof Kernel (ZPK)**:
The umbrella contract for the four future Zith proof sub-systems NIA, RRA, MRA and NRA.
`ZPK` is not one pass; it names the coordinated set and their shared facts.
_Avoid_: safety module, proof model, compiler analyzers

**NIA**:
Numeric Interval Analysis, the ZPK sub-system that collects and proves numeric facts, ranges, versions, control-flow joins and function headers.
_Avoid_: NTA, numeric analysis, facts module

**RRA**:
Region Relationship Analysis, the ZPK sub-system that proves geometry for slices, arrays, region ranges, pool slots and allocation blocks.
_Avoid_: geometry pass, region checker

**MRA**:
Memory Region Analysis, the ZPK sub-system that declares and controls static region, heap and pool shapes, permissions, stream typing and allocator provenance.
_Avoid_: memory map, address checker, allocator runtime

**NRA**:
Node Resource Analysis, the ZPK sub-system that proves ownership, lifetime, borrow and escape facts for resources.
_Avoid_: borrow checker, ownership checker

**AggregateNode**:
The NRA resource graph node for a whole value that owns or tracks storage. It is a container of per-field nodes and owns the aggregate-level move, borrow and escape edges; individual fields appear as child nodes only when they carry ownership-relevant metadata.
_Avoid_: bigNode, large node, container node

**FieldNode**:
An NRA resource graph child node created only for a field with ownership significance (reference, bind, own, pointer or resource type). A plain scalar field does not become a node.
_Avoid_: field record, member node, smallNode

**Reference (`&`)**:
A non-owning, region-bound access to a resource. `&T` permits reads, `&mut T`
permits writes, and a live reference pins its source against relocation and
consumption. The canonical spelling follows mutability: `&T` for the read
reference and `lend T` for the write reference. `view T` is a non-canonical
alias of `&T`, and `&mut T` is a non-canonical alias of `lend T`.
_Avoid_: raw pointer

**Bind (`^`)**:
A non-owning lifetime dependency without a region limit or pin. It becomes
invalid when its target is consumed or relocated, and NRA reports that
invalidation at the next use.
_Avoid_: raw pointer, `belong`, weak reference

**Own (`%`)**:
Logical ownership of an address or slot in the full-Zith reference model.
Moving it marks the source binding dead.
_Avoid_: `unique`, owning pointer

**Resource identity**:
The stable NRA identity of a resource, independent of the binding/symbol that currently names it. Moving `q = p` rebinds the name `q` to the existing resource node and leaves `p` dead; it does not copy or recreate the resource graph.
_Avoid_: variable identity, symbol identity, value identity

**Aggregate move**:
Moving an entire aggregate requires every ownership-relevant FieldNode to be alive at the move point. The move itself only re-targets the symbol to the AggregateNode and marks the old symbol dead.
_Avoid_: struct copy, container move, whole-value transfer

**Physical move**:
The relocation of a resource between struct fields or compatible value slots. The source becomes uninitialized and only the destination remains valid; the source can be restored with `=`.
_Avoid_: shallow copy, bit copy, value relocation

**Logical move**:
The transfer or consumption of a storage identity, address, or ownership edge through an ownership-qualified binding. The old symbol cannot be restored by writing through it; `:=` creates a new aggregate/resource identity.
_Avoid_: address move, ownership move, slot move

**Partial field move**:
Moving one field out of an aggregate consumes only that FieldNode and leaves sibling FieldNodes alive; a subsequent whole-aggregate move is then rejected until the consumed FieldNode is restored.
_Avoid_: field extraction, partial copy, member move

**Execution flow**:
A control-flow lineage used by NRA to distinguish ordinary sequential execution from an explicitly forked execution branch. Read/write coexistence is allowed within one flow; a thread fork creates a new flow and requires ownership/share rules to prevent conflicting cross-flow access.
_Avoid_: thread context, runtime flow, execution path

**Bounded flow**:
An execution flow whose lifetime is scoped by its parent and whose resources can be joined or merged before the parent scope ends.
_Avoid_: normal thread, scoped task

**Unbounded flow**:
An execution flow that may outlive its parent. Borrowed resources crossing into it require an explicit revocable contract.
_Avoid_: detached ownership, background task

**Revokable access**:
The full-Zith resource-access contract whose availability may be revoked by its
parent flow. It is a reference whose access can be revoked, not a revocable
value.
ADR-0034 fixes the source spelling as a type qualifier, not a wrapper type. The
sigil `'` or the long form `grant` precedes any ownership qualifier except a
bare `T`, so `'&T`, `'&mut T`, `'^T`, `'^mut T`, `'%T`, and `'%mut T` all
exist. A bare `'T` does not exist because a revocable `default` would imply a
logical move of an inline value. The sigil is written only in type position.
The `unbounded` launch site marks where revocation happens, so a call argument
carries no revocable annotation.
_Avoid_: revocable pointer, nullable borrow, cancellable reference

**Waiter**:
The minimal synchronization contract passed to a thread flow. It exposes `wait()`; the thread implementation decides whether waiting means joining, merging, or waiting for revocation completion.
_Avoid_: thread handle, future, join token

**Node provenance**:
The resource-identity relation carried by a returned value or field. A result may identify an existing node directly, including a specific FieldNode, rather than merely recording the argument position from which it originated.
_Avoid_: return alias metadata, return source, argument provenance

**Region**:
A static MRA declaration of contiguous memory with a known or symbolic shape, used for hardware, arenas and scratch memory.
_Avoid_: memory area, mmap, heap

**Heap**:
A separate MRA keyword for a dynamic allocation domain. The base may be `unknown` at compile time and the total size is `dynamic`; allocated blocks carry their own length.
_Avoid_: region, arena, malloc domain

**Pool**:
A static MRA declaration for a fixed-count typed region. `P`, `T`, and `N` derive `size`, `stride`, `alignment`, and slot facts; the declaration should not repeat derivable fields.
_Avoid_: typed region, block allocator region, object arena

**Block<R>**:
An allocation result carrying a region-provenance pointer and a byte length: `Block<R> { ptr: Ptr<R>, len: u64 }`.
_Avoid_: pointer with size, memory block, raw allocation

**Ptr<R>**:
A pointer object whose provenance is a comptime MRA region, heap, or pool named `R`.
_Avoid_: raw pointer, tagged pointer, region pointer

**Slot access**:
The MRA access to a fixed-count `pool P(T, N)` through `@poolSlot(P, index)`. The compiler derives `index < count` from the declaration, so RRA can prove `Disjoint(slot(k1), slot(k2))` when NIA proves `k1 != k2`.
_Avoid_: pool indexing, raw slot arithmetic, block slot

**Region access result**:
`@regionAt` may return `Ptr<R>` for a unique element or `Block<R>`/slice for an interval. A `Ptr` is the identity of one element; a `Block`/slice is an interval of elements/bytes.
_Avoid_: pointer, access span, region value

**Init**:
The single-site MRA transition that resolves a region/heap/pool with `base: unknown` to a runtime address and records its domain size; it is performed by `@regionInit(region, addr, size)`.
_Avoid_: constructor, setup, initialize permission

**Region access**:
The MRA expression that reads or writes a region through `@regionAt(region, offset)`. Typed streams follow their declared element type; untyped streams behave as raw byte streams.
_Avoid_: pointer arithmetic, raw cast, device access

**Bump no-op free**:
The bump allocator contract where `free(block)` does not reclaim the block. NRA treats the block as valid until region reset/scope end, not as freed.
_Avoid_: bump free, arena free, individual deallocation

**Contract intrinsic**:
One of `@assume`, `@ensure`, or `@maybe`: a ZPK-level construct that lets programmers and allocator implementations declare proof boundaries without blurring subsystem responsibilities.
_Avoid_: assertion, compiler hint, proof pragma

**Assume**:
The trusted-premise intrinsic `@assume(cond)`. Unless the current environment already proves `cond` false, installs `cond` as `True` in that scope; a known contradiction is a diagnostic. It is an audit-marked escape hatch, not a verified assertion.
_Avoid_: verify, assertion, assume if proved

**Ensure**:
The caller-side contract intrinsic `@ensure(cond)`. A function/capability declares that any caller must satisfy `cond` before the call; ZPK checks or derives the caller premise and records it as a contract fact.
_Avoid_: postcondition, guarantee, verifier promise

**Maybe gate**:
The intrinsic `@maybe(cond)`, valid only when the current analysis state for `cond` is exactly `Maybe`. `Unknown`, `True`, and `False` are rejected; the caller can consume the fuzzy fact conservatively.
_Avoid_: fallback predicate, optional condition, unknown check

**Cached premise header**:
The collected, cached contract facts for a function or capability. Normal callers verify these premises at the call site instead of reanalyzing the callee; each ZPK sub-system checks only the premises in its own domain.
_Avoid_: function annotation, inline contract, global proof cache

## Opaque Identity

**Canonical type id**:
The stable 128-bit identity of a Zith type, derived from its defining module namespace, canonical field order, and type name, used to hydrate `opaque` values.
_Avoid_: typeId, tag hash, deterministic hash id

**Runtime opaque tag**:
The project-local `u32` assigned by the compiler for a canonical type id and stored in the opaque runtime representation.
_Avoid_: global type id, canonical runtime id, stable tag

**Canonical type registry**:
The persistent project cache file that records which canonical type ids already have runtime opaque tags, used to keep later builds stable.
_Avoid_: global registry, runtime type table, tag table

**Canonical opaque mapping**:
The serialized per-artifact pair `{ canonical type id, runtime opaque tag }` that lets a cached opaque value be hydrated with its original tag.
_Avoid_: opaque metadata, tag recovery record, cache typehash

## Toolchain & C Interop

**Standalone toolchain**:
The packaging model where `zithc` carries the tools it needs to build native programs, so an end user installs only the compiler artifact instead of a separate host compiler toolchain.
_Avoid_: bundled compilers, host toolchain, toolchain folder

**Embedded LLD**:
The LLD linker linked into `zithcLib` and invoked through the LLD API in-process instead of executing `ld.lld` from the host.
_Avoid_: system linker, external LLD, ld.lld driver

**Host C driver**:
The external C compiler used as a temporary fallback for `*.c` compilation until Zith ships its own lightweight C backend.
_Avoid_: system compiler, user compiler, C toolchain

**Zith C binder**:
The future embedded, Zith-owned parser that consumes C headers and produces the same `cinterop` binding surface libclang produces today, without requiring LLVM/libclang.
_Avoid_: C parser, libclang replacement, mini-clang

**Tiny C backend**:
The long-term lightweight C compiler/object emitter owned by Zith for companion C sources and for standalone C system-header support. It prioritizes small footprint, stability, and correct simple C over optimization.
_Avoid_: TinyCC backend, tinyc clone, mini C compiler

**Validated C ABI surface**:
The exact set of C declarations Zith promises to bind with correct ABI. Declarations outside this surface are rejected with a clear diagnostic, not imported with unverified semantics.
_Avoid_: arbitrary C interop, experimental C import, best-effort ABI

**WASI header bundle**:
The small set of C library headers shipped with the WASM build so `import "foo.h"` works in the browser without relying on host system headers.
_Avoid_: system headers for WASM, browser libc headers, precompiled headers

**Bundle provider**:
The configurable source of embedded/locally installed compiler resources (LLD, header resource dirs, Zith-owned C backend, WASI headers) resolved relative to the `zithc` binary.
_Avoid_: toolchain root, LLVM_DIR, compiler path

**C++ interop slice**:
The only defensible C++ interop scope: Itanium name mangling plus standard-layout class ABI compatible with C, with no vtables, RTTI, exceptions, or templates. It is parked behind a completed and verified C ABI and a concrete C++ library to call.
_Avoid_: full C++ ABI, Itanium ABI support, C++ frontend
