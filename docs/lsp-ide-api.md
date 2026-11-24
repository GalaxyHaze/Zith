# Zith IDE API

Status: accepted
Version: 1.0

## Scope

This document is the source of truth for the `zith::ide` C++ API, the
IDE-facing boundary used by Zith ecosystem components such as `zith-lsp`,
`zith-extension/vs-code`, and WASM playground consumers.

The compiler's public ABI remains `src/capi/zithc-capi.h`. External consumers
that are not an in-tree Zith ecosystem component MUST use either LSP/JSON-RPC
or the C ABI. External consumers MUST NOT include internal compiler headers
from `src/session/`, `src/frontend/`, `src/sema/`, `src/symbols/`, or
`src/types/`.

`zith::ide` is the supported C++ boundary for Zith ecosystem components. It is
not a general compiler API and is not intended for end-user applications that
could use `zithc-capi.h` instead.

## Lifecycle

`zith::ide::Workspace` owns the shared frontend/semantic context used for
analysis. Consumers open a workspace root once, then open documents and submit
analysis requests.

Each analysis runs against an immutable `zith::ide::DocumentSnapshot`. The
snapshot contains the in-memory document text and the version supplied by the
consumer. Saving a document MUST NOT invalidate a newer in-memory revision when
the consumer already applied it to the same `DocumentSnapshot`.

Cancellation is cooperative. A request may check a cancellation token. Once
cancelled it returns an abandoned result rather than publishing partial data.
`Workspace` keeps the frontend context owned by the facade. Pointers to AST
nodes, `frontend::FrontendSnapshot`, `sema::modern::TypedMap`, or
`session::CompilationSession` MUST NOT escape the facade.

## Best-Effort Analysis

Analysis results are best-effort when the source is incomplete. Every
`AnalysisResult` carries `analyzedRevision`, the exact revision that produced
it. If the requested revision cannot be analyzed, the implementation MUST fall
back to the last valid `DocumentSnapshot` for the same document and MUST NOT
invent results. Stale results MUST NOT be published for a newer revision.

## Features

The contract enumerates the IDE queries implemented by `zith::ide`. An
implementation may expose a stable subset as long as capabilities not present
in that subset are not announced to clients. Feature statuses use:

- `stable`: part of `zith::ide` v1.0 and preserved under minor releases.
- `experimental`: may change without breaking stable `zith::ide` callers.
- `planned`: contract reserved for a later major/minor and not announced.

The stable surface includes:

- diagnostics
- completion and completion item resolve
- hover
- signature help
- definition
- references
- document symbols
- workspace symbols
- semantic tokens full and delta
- inlay hints
- document highlights
- formatting
- folding ranges
- code actions
- workspace commands
- configuration

Member completion returns fields and methods applicable to the static type of
the receiver and visible at the request location. Members hidden by private or
module-level visibility rules are omitted.

Document symbols list the top-level declarations of the open document and, for
a `fn` or `state` body, its named blocks as outline children. A named block's
`detail` carries its marker and resource arguments (for example `@scoped(r)`),
so a reader sees what each phase brackets without opening the body. A plain
named block has no detail.

The following features are experimental in this contract:

- semantic token delta under cancellation
- workspace commands beyond the current static set
- cross-file did-change-watched-files indexing details

The following features are planned and not announced by the LSP in v1.0:

- WASM JSON-RPC transport
- language server `configuration` change subscriptions beyond current static
  initialization options

## Versioning

The Zith IDE API is versioned as `1.0`. Additive, optional features are allowed
in minor versions. Removals and breaking changes require a major version.
Deprecations are announced one major version before removal.

The schema in `lsp-ide-api.schema.json` is authoritative for JSON payloads that
cross the `zith::ide` boundary. The schema and this document must agree with
the implementation before a merge.

## LSP Relationship

`zith-lsp` documents describe how IDE features are exposed over the LSP
transport. They are implementation guides. The contract shape, revision
semantics, capability policy, and C++ ownership rules live here.
