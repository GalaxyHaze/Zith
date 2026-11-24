# Zith-- Beta Experience Roadmap

Objective: prepare a useful, accurately documented Zith-- beta by proving its
module ABI and runtime opaque-tag identity guarantees, then polishing the
compiler's user-facing path through `zith-lsp`, the VS Code extension, the
website, and the shipped standard library. The ABI and identity work is a
bounded, release-blocking compiler track. It does not authorize a general
compiler redesign or a first-party IDE project.

Status: proposed. This roadmap is a release-readiness plan, not a promise that
every feature in either language specification will be implemented.

Planning estimate: the previous 4 to 6 week estimate covered the user-facing
beta work only and is superseded as a total estimate. The ABI and runtime
identity track has not been estimated. Step 2 must produce a revised range
before implementation begins. This is not a fixed release deadline. Do not
shorten the plan by dropping a gate.

Preconditions:

- Work from `/home/diogo/Zith`.
- Run every build and test through `agent-run`.
- Reuse `/home/diogo/Zith/build`. Do not reconfigure or delete it.
- Before making new changes in the LSP, extension, or website repositories,
  review their existing changes, run the relevant focused tests, and commit the
  intended baseline work separately. Preserve unrelated or unexplained changes.
- Use only reviewed, committed revisions when preparing beta artifacts.
- This planning task modifies only
  `/home/diogo/Zith/docs/plans/zith-minusminus-beta-roadmap.md` and
  `/home/diogo/Zith/docs/deltas/generic-abi-type-identity.md`. Keep the delta
  under review. Do not import it into the canonical specification or create
  its ADR until the review is complete and the project owner approves those
  next steps.
- After any implementation change, immediately run the affected focused test
  before beginning another change. Run the full release gates again on the
  final candidate revisions.

## Scope And Baseline

The beta targets `Zith--`, a sibling language to Zith. Its contract is
`docs/Zith--.md`, its implementation rules are in
`docs/Zith---implementation.md`, and `docs/impl-status.md` records observed
compiler behavior. `docs/Zith-spec.md` describes Zith, not a larger version of
the Zith-- contract. Do not present Zith-only proposals as Zith-- capabilities.

The work includes one compiler track for stable module ABI and project-local
runtime opaque-tag identity, plus a reliable first-use experience, accurate
claims, reproducible installation and checks, and a small, explicit
standard-library surface. Keep the existing compiler architecture. Do not
build a first-party IDE, redesign unrelated compiler subsystems, or add broad
language or library features for this beta.

Approved beta-surface decisions:

- macOS and Linux musl are outside this beta's support scope. Do not claim
  either as supported or include either in the supported beta matrix,
  regardless of whether an artifact can be built.
- Do not imply full or general platform support. Any other operating-system
  and architecture combination remains a candidate, not supported, until its
  exact compiler and LSP artifacts, installation route, native smoke test, and
  end-to-end first-use path pass on that target.
- `c/` is a thin, simple portability layer over C, not a safe wrapper.
  `std` contains only modules needed by verified examples and first-use tasks.
  `soon/` remains experimental and is not part of the stable beta surface.
- The five first-use tasks and user profile below define the beta acceptance
  experience. They do not imply support for a platform until that platform
  passes the native evidence gate.

The intended user is new to Zith-- but has some familiarity with a systems
language. Assume the reader can use a terminal and build tools and understands
basic types, functions, imports, and explicit resource management. Do not
assume prior knowledge of Zith--, this repository, or compiler implementation.

At execution start, capture the state of all four repositories. Do not rely on
older commit counts, branch names, or dirty-file lists as the current baseline.
Review existing changes in `zith-lsp`, `zith-extension`, and `Zith-website`,
confirm which changes are intended, run their focused tests, and commit the
reviewed baseline changes separately before starting new work. Never discard,
reset, overwrite, or silently include existing work.

### Current CI Evidence Gaps

This is a review of checked-in workflows and test sources as of 2026-11-20,
not evidence that a workflow has passed or that every artifact works at
runtime. Recheck each item against the selected candidate:

- The compiler release workflow builds artifacts for Linux, macOS, and Windows
  across multiple architectures. macOS is outside this beta's support scope.
  Installer smoke workflows cover Ubuntu, macOS, Windows, and a WASM bundle,
  but do not demonstrate native Windows ARM64 execution. See
  `.github/workflows/build-artifact.yml` and
  `.github/workflows/smoke-installers.yml`.
- The LSP's routine test workflow runs on Linux amd64. Its release workflow
  builds additional targets without running the same LSP test suite on each.
  See `/home/diogo/zith-lsp/.github/workflows/test.yml` and
  `/home/diogo/zith-lsp/.github/workflows/build-artifact.yml`.
- The extension's downloader tests mock OS and architecture combinations.
  They verify asset-name selection, not native execution. The VSIX host harness
  is Linux-specific and is not part of `npm test`. See
  `/home/diogo/zith-extension/vs-code/test/unit/test-download.js` and
  `/home/diogo/zith-extension/vs-code/test/integration/run-vsix-host.js`.
- The website workflow builds and checks the site but does not run the
  documented routing and editor Node tests. See
  `/home/diogo/Zith-website/.github/workflows/build.yml`.
- The extension downloader requests `zith-lsp-macos-universal` from
  `GalaxyHaze/Zith-Lang`. The checked-in release workflows do not establish
  that this exact asset is published in that repository. macOS is excluded
  from this beta, so this mismatch is not a beta blocker unless the scope is
  later changed. See
  `/home/diogo/zith-extension/shared/scripts/download-lsp.js` and the
  LSP/compiler release workflows.
- Linux musl is also outside this beta's support scope. Do not infer Linux
  support for musl from a Linux artifact or a mocked platform-selection test.

Builds, mocked selection tests, and declared artifacts are not equivalent to
native runtime or end-to-end support. Mark each matrix row with the evidence
actually collected.

Existing work to carry forward rather than duplicate:

- `docs/plans/zith-minusminus-publicity.md` owns the introduction article and
  publication-specific checks.
- `docs/review-website-inconsistencies-2026-11-02.md` records website
  discrepancies marked critical on 2026-11-02. Revalidate each item against
  current source before fixing or closing it.
- `docs/vscode-extension-lsp-api.md` is the existing LSP-to-extension
  handoff. Reconcile it with the current LSP and extension rather than
  treating its feature checklist as proof of support.
- `docs/plans/stdlib/soon-promotion.md` defines the experimental-module
  promotion work. Do not promote modules to `std` just to enlarge the beta
  feature list.

## Beta Acceptance Gates

The beta is ready only when every gate below passes:

1. **Truth:** every feature claimed on the website, in the extension, or in
   quick-start material is supported by the current Zith-- compiler and has
   reproducible evidence. Every row in `docs/impl-status.md` has a checked
   disposition. No critical or high-priority public-claim discrepancy remains
   open.
2. **Editor journey:** a clean VS Code profile can install the packaged
   extension, open a Zith-- project, receive and clear diagnostics, use the
   advertised editor features, and run the documented check/run path without
   manually locating a development binary.
3. **Website journey:** the site builds, its routing and editor tests pass,
   and a new reader can reach the specification, implementation status, a
   verified example, and the installation path. Every route enumerated by the
   routing test is checked at 375x812 and 1440x900 viewports, with no
   horizontal overflow, broken primary navigation, or browser-console error.
   Do not deploy as part of this roadmap.
4. **Library:** the beta's shipped module list is explicit. Every module
   described as supported passes isolated type checking and has runtime
   coverage for each behavior promised in its documentation. `soon/` remains
   experimental and `c/` remains the raw C binding layer.
5. **Diagnostics:** representative invalid source produces a precise,
   understandable diagnostic that identifies the problem and offers a
   correction grounded in the language contract. Use respectful language and
   avoid blame, unexplained compiler jargon, and generic messages. The
   highlighted span points to the source that needs attention, and recovery
   does not add misleading cascaded errors. Identical diagnostics are not
   duplicated in one playground result, and a later successful compile clears
   stale errors.
6. **Platform support:** a compiler target is supported only when its
   installation route and documented check path pass on that target, and the
   corresponding LSP and extension artifact are verified through the
   advertised editor workflow. Native runs are required for every platform
   claimed as supported. Artifact builds and mocked OS/architecture selection
   tests alone do not pass this gate. Mark all other combinations as
   unverified or unsupported, and document their impact.
7. **ABI and runtime identity:** the ABI and identity track in Step 2 passes
   on every supported beta target. Generic symbol identities distinguish
   modules, generic owners, explicit trait implementations, signatures, and
   argument arities without using runtime `u32` tags. Cache round trips use
   structured instantiation metadata. Runtime opaque tags remain unchanged
   across warm hydration, cold artifact rebuilds that preserve durable identity
   state, module-order permutations, and module addition or removal. Tests
   prove that clearing disposable module artifacts does not clear the durable
   identity state. Incompatible public ABI changes require a version change.
   Do not claim cache-artifact compatibility across compiler versions.
8. **Pilot:** before public release, at least three and up to five first-time
   Zith-- users who meet the profile above complete the five acceptance tasks
   without verbal help. Record their results and triage every finding. Fewer
   than three participants or untriaged blocking findings fail this gate.
9. **Release candidate:** the compiler build, full CTest suite, example smoke
   check, LSP tests, extension-host tests, website build, website tests, and
   install/package smoke checks all pass on the exact reviewed candidate
   revisions for every platform in the supported matrix. Record the revision
   IDs and logs. No critical or high-priority claim discrepancy, broken
   acceptance task, crash, missing/mismatched advertised artifact, or failed
   ABI and runtime identity gate remains. Document lower-priority limitations
   with their impact and workaround.

## Roadmap

### 1. Freeze The Beta Surface And Baseline

**Window:** Week 1.

**Goal:** freeze a bounded Zith-- beta surface, capture the exact starting
state of every repository, and establish evidence-based support claims before
new cross-repository work begins.

**Actions:**

1. Record `git status --short --branch` and `git rev-parse HEAD` in
   `/home/diogo/Zith`, `/home/diogo/zith-lsp`,
   `/home/diogo/zith-extension`, and `/home/diogo/Zith-website`.
2. Review tracked and untracked changes in each related repository without
   discarding, overwriting, or staging any existing work. Record which changes
   are intended. If intent is unknown, mark that repository blocked and do not
   run tests that could modify its worktree.
3. Run the focused baseline tests in each repository only after its existing
   changes are understood. Record the exact commands, revisions, and outcomes.
   If a test fails, preserve the output and stop new work in that repository.
4. Commit only reviewed, intended baseline changes, separately in their owning
   repositories. Do not include generated files or unrelated changes. If
   intent is unknown or a focused test fails, leave the worktree unchanged and
   record the blocker instead of committing.
5. Freeze the beta surface as Zith-- only. Keep macOS and Linux musl outside
   the beta support matrix. Do not claim any other target as supported until
   its exact compiler and LSP artifacts, installation route, native smoke
   test, and end-to-end first-use path have passed on that target.
6. Define the user profile as a person new to Zith-- who can use a terminal,
   build tools, and basic systems-language concepts, but does not need
   compiler implementation experience.
7. Record candidate compiler targets, installation routes, LSP assets,
   extension download sources, and standard-library modules. For every
   candidate platform and architecture, separately mark artifact built,
   selection mocked, native smoke-tested, and end-to-end tested. These are
   evidence states, not support claims.
8. Use five first-use tasks as the acceptance script: install the extension,
   open a project, observe an actionable error diagnostic and clear it by
   correcting the source, get one advertised editor result such as completion
   or hover, and successfully check or run the verified example. Record an
   expected result and pass/fail evidence for each task.
9. Keep unrelated language features, broad standard-library growth, compiler
   architecture changes outside the ABI and identity track, and a standalone
   IDE out of scope. Treat `c/` as a thin, simple portability layer rather
   than a safe wrapper. Limit stable `std` modules to verified examples and
   first-use tasks, and keep `soon/` experimental. The ABI and identity track
   is the explicit compiler-work exception and is mandatory for publication.

**Commands:**

```bash
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith status --short --branch
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith rev-parse HEAD
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-lsp diff --check
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-lsp diff --cached --check
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-lsp status --short --branch
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-lsp rev-parse HEAD
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-extension diff --check
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-extension diff --cached --check
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-extension status --short --branch
agent-run -t 30 -c 0 -- git -C /home/diogo/zith-extension rev-parse HEAD
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith-website diff --check
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith-website diff --cached --check
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith-website status --short --branch
agent-run -t 30 -c 0 -- git -C /home/diogo/Zith-website rev-parse HEAD
agent-run -t 900 -c 0 -- cmake --build /home/diogo/zith-lsp/build -j4
agent-run -t 900 -c 0 -- bash -lc 'cd /home/diogo/zith-lsp && python3 -m unittest discover -s tests -p "test_lsp.py"'
agent-run -t 300 -c 0 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && npm test'
agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
agent-run -t 30 -c 0 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
agent-run -t 30 -c 0 -- node /home/diogo/Zith-website/tests/docs-editor.test.js
```

**Checks and evidence:**

- All four repository revisions and dirty-file lists are recorded in the
  execution table under Final Acceptance Record.
- Existing changes in the three related repositories are classified as
  intended, unrelated, or unresolved. Focused tests pass for each reviewed
  baseline, and intended baseline changes are committed separately before
  new work begins. An unresolved repository is explicitly blocked.
- The support matrix names candidate platforms and architectures, artifacts,
  installation routes, evidence states, and exact end-to-end results. macOS
  and Linux musl are absent from the supported beta set.
- The acceptance script has five numbered tasks, expected results, and a
  pass/fail field. It also records the assumed user profile.

**Failure checks:**

- If a related repository has changes of unknown intent or failing focused
  tests, do not begin new work or package artifacts from it. Preserve its
  current worktree and record the exact blocker.
- If macOS or Linux musl appears in a beta support claim, remove that claim.
- If another platform lacks native end-to-end evidence, mark it a candidate,
  unverified, or unsupported. Do not infer support from an artifact build or
  mocked test.
- If any proposed work requires a language redesign or a new IDE, defer it
  beyond beta.

**Success criterion:** all baseline revisions and intended existing changes
are recorded, the related baseline changes are committed, and the user profile,
support matrix, and five-task acceptance script are reviewed. The supported
set excludes macOS and Linux musl and contains no other target without native
end-to-end evidence.

### 2. Reconcile The Specification, Status, And Compiler

**Window:** Weeks 1-2 for the audit. The ABI and runtime identity track begins
after its delta is reviewed and continues until its release gate passes. Its
implementation duration is not yet estimated and may extend the candidate
window.

**Goal:** establish one evidence-backed account of current Zith-- behavior,
then complete the release-blocking ABI and runtime identity track before beta
publication.

**Actions:**

1. Recheck `docs/impl-status.md`, whose recorded broad audit predates this
   roadmap, against the compiler at the approved candidate revision.
2. Use `docs/Zith--.md` as the language contract. Consult
   `docs/Zith---implementation.md` for implementation rules. Consult
   `docs/Zith-spec.md` only to catch accidental conflation with the separate
   Zith design.
3. Create a discrepancy table with one row per status or public-claim issue.
   Record the feature, source section, status entry, compiler evidence
   (test name or exact command and result), discrepancy, decision, and
   resolution.
4. For every `Working` status used in beta material, identify at least one
   passing test or reproducible compiler example. For `Partial`, `Parse-level
   in progress`, `Parse skipped`, `Parse error`, `Recognized only`, `Check
   only`, `Stub`, and `Spec only`, preserve the limitation in any public
   description.
5. Add focused regression cases for the malformed import forms
   `import from path` and `import / from path{...}`. Keep them invalid if that
   matches the language contract. Require the diagnostic to explain the
   problem and point to the documented alternatives: `import path` binds a
   module namespace, while `from path` imports public symbols directly. If
   the intended form is ambiguous, explain both choices instead of guessing.
6. Add a regression case for `if (cond) expr` when the language contract
   requires a braced body. Keep the syntax invalid and require the diagnostic
   to explain that a block is expected and show the correction using `{ ... }`.
7. Verify each case's source span, primary message, and suggestions. Reject
   generic or blaming messages, unexplained compiler jargon, and cascading
   diagnostics that obscure the first error.
8. Extend `tests/test-wasm-playground.cpp` to submit the same malformed source
   repeatedly through each applicable compile/run entrypoint and verify each
   result contains the same diagnostic set without accumulating duplicates.
   Submit a corrected source and verify the diagnostics array is empty.
   Preserve distinct diagnostics when they report different problems.
9. Re-audit every item in
   `docs/review-website-inconsistencies-2026-11-02.md`. Mark it fixed with
   evidence, no longer applicable with evidence, or deferred with an explicit
   reason. Do not copy an old recommendation without checking the current
   website and compiler.
10. Update `docs/impl-status.md` only after behavior has been checked. Update
   the dated website review and linked documentation to reflect the decision.

**Commands:**

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build --output-on-failure
agent-run -t 30 -c 50 -- /home/diogo/Zith/build/zithc check /home/diogo/Zith/examples/loops-simple.zith
```

**Expected output:** all commands exit `0`. The discrepancy table covers every
status row and every previously recorded public-facing finding. The malformed
import and control-flow fixtures produce the expected actionable diagnostic,
span, and correction without misleading duplicates or cascades. Repeated
playground calls do not accumulate identical diagnostics, and successful calls
clear stale errors.

**Failure checks:**

- If the compiler contradicts a `Working` status, keep the feature out of
  beta claims, add a focused regression test, and correct the status only
  after the behavior is fixed or accurately classified.
- If a test fails for an unrelated reason, record the failure and block the
  release candidate until it is resolved or explicitly proven unrelated.
- If a website claim cannot be checked, remove or defer that claim. Do not
  infer support from syntax highlighting, a planned API, or a parser-only test.
- If a proposed diagnostic suggests the wrong import form or changes the
  language contract, correct the message or defer it. Do not make invalid
  syntax valid as a diagnostic shortcut.

**Success criterion:** there are zero unresolved critical/high public-claim
discrepancies, each status row has a checked result or an explicit non-working
disposition, and the focused malformed-source fixtures have actionable,
non-duplicated diagnostics. The compiler ABI and runtime identity sub-track
below must also pass before the release candidate can be approved.

#### Release-Blocking Compiler ABI And Runtime Identity Track

**Window:** Starts after review of
`docs/deltas/generic-abi-type-identity.md`. Estimate the implementation range
after the design inventory in this track. Completion is required before beta
publication.

**Goal:** make generic symbol identity collision-free and versioned, keep
runtime opaque tags stable for a project with its durable identity state, and
produce repeatable evidence on every supported beta target.

**Actions:**

1. Compare the ABI delta with its canonical target:
   `docs/Zith--.md#objetivo`, `docs/Zith--.md#opaque-tagged`, and
   `docs/impl-status.md#functions-bindings`. Review the delta with the project
   owner before importing it. Do not create the ABI ADR before the delta is
   finalized.
2. After the delta is finalized, record the implementation choices in the
   next sequential ADR under `/home/diogo/Zith/docs/adr/`. The ADR must decide
   the target-safe symbol text encoding, the durable registry's storage and
   lifecycle, first-build assignment determinism, and module/artifact ABI
   versioning. Do not start implementation while any of those choices is
   implicit.
3. Add or extend focused regressions in
   `/home/diogo/Zith/tests/test-hir-lower-modern.cpp`,
   `/home/diogo/Zith/tests/test-cache.cpp`, and
   `/home/diogo/Zith/tests/test-codegen.cpp`. Cover two generic owners with
   the same method and concrete type arguments, same-named methods from
   explicit trait implementations defined in different modules, generic
   argument arity, non-generic names, and the `extern fn`, `= extern`, and
   `main` exceptions.
4. Assert that generic methods carry the defining module and owner identity
   once, use declaration template types for parameter spelling, omit implicit
   `self` and return type from symbol identity, and order canonical owner
   arguments before function arguments. Assert that symbols use 128-bit
   canonical type identities rather than project-local runtime `u32` tags.
5. Store generic-instantiation identity as structured cache metadata. Add a
   round-trip regression proving that cache hydration does not parse generic
   arguments from the emitted symbol string.
6. Add registry tests for a warm session, a cold artifact rebuild with durable
   identity state retained, different module compilation orders on an empty
   registry, and adding/removing/reintroducing modules after tags are assigned.
   Existing tags must remain unchanged. Clearing disposable module artifacts
   must not clear the durable identity state.
7. Validate the final symbol encoding on every target retained in the beta
   support matrix. If a target cannot represent the encoding, resolve a
   target-specific encoding before the ABI is frozen or remove that target
   from the supported matrix.
8. Version public ABI changes. Verify that a changed symbol, calling
   convention, or public layout cannot be consumed as the same module/artifact
   ABI version. Do not require retention of old function symbol copies.
9. Run the focused compiler tests, then the full build and CTest suite. Record
   the compiler revision, target, exact commands, and results in the final
   acceptance record.

**Commands:**

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4 --target test-hir-lower-modern test-cache test-codegen
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^(test-hir-lower-modern|test-cache|test-codegen)$' --output-on-failure
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build --output-on-failure
```

**Expected output:** every focused regression passes, including cold/warm
identity and module-order cases. The full build and CTest suite exit `0`.
Every supported target emits distinct generic symbols for distinct
instantiations, and the recorded ABI version changes for incompatible public
ABI changes.

**Failure checks:**

- If two distinct declarations or instantiations emit the same symbol, keep
  the beta gate failed and add a regression for the exact collision.
- If a module-order permutation assigns different tags to the same canonical
  type in a clean initial registry, keep the beta gate failed. Do not claim
  deterministic runtime identities.
- If clearing disposable cache artifacts loses an existing type-to-tag
  assignment, keep the beta gate failed. Resolve durable storage before
  publication.
- If cache hydration needs to parse generic arguments from symbol text, keep
  the cache portion of the gate failed and store the metadata structurally.
- If a supported target cannot represent the chosen symbol encoding, do not
  freeze the ABI or advertise that target until the encoding or support matrix
  changes and is retested.
- If any ABI, runtime identity, or full-suite test is unrun or fails, do not
  approve the beta candidate.

**Success criterion:** all focused and full tests pass on every supported
target, generic symbol identities are collision-free, the ABI version changes
for incompatible public changes, and persistent runtime opaque tags satisfy
every cold/warm, order, and module-lifecycle fixture.

### 3. Polish `zith-lsp` And The VS Code Extension

**Window:** Weeks 2-4.

**Goal:** make the supported editor workflow predictable from the packaged
extension, not only from a developer checkout.

**Actions:**

1. Review the baseline commits from Step 1 in
   `/home/diogo/zith-lsp` and `/home/diogo/zith-extension`. Use only reviewed,
   committed revisions for beta artifacts.
2. Compare server capabilities with
   `/home/diogo/Zith/docs/lsp-ide-api.md` and
   `/home/diogo/Zith/docs/vscode-extension-lsp-api.md`. Advertise only
   capabilities implemented and tested by the server.
3. For every proposed supported platform and architecture, verify that the
   extension requests an artifact that exists in the configured release
   repository and matches the target. Verify the installed compiler and LSP
   natively on that target. Test every claimed route end to end. The
   `zith-lsp-macos-universal` asset mismatch is outside this beta's scope
   because macOS is excluded. Reopen that mismatch only if the approved beta
   support scope changes.
4. Make the five first-use tasks from Step 1 pass through a clean VS Code
   profile and the packaged VSIX. Include project discovery, diagnostics
   appearing after a malformed import or unbraced control-flow body and
   clearing after a fix, one advertised navigation/intelligence feature, and
   the documented check/run command.
5. Verify failures are actionable: missing server, missing stdlib, invalid
   project, compiler error, and server restart each produce a visible message
   in the editor or `Zith LSP` output channel.
6. Verify repeated edits do not leave duplicate or stale diagnostics in the
   editor. The editor must clear the original diagnostic after the source is
   corrected.
7. Verify the packaged server is the reviewed server build. Record SHA-256
   values for the packaged and installed server binaries.
8. Update the LSP/VS Code handoff document with the tested command names,
   capabilities, settings, and known limitations.

**Commands:**

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/zith-lsp/build -j4
agent-run -t 900 -c 0 -- bash -lc 'cd /home/diogo/zith-lsp && python3 -m unittest discover -s tests -p "test_lsp.py"'
agent-run -t 300 -c 50 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && npm test'
agent-run -t 300 -c 50 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && node test/integration/run-host.js'
agent-run -t 120 -c 50 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && vsce package --out /tmp/zith-language-beta.vsix'
agent-run -t 300 -c 50 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && node test/integration/run-vsix-host.js /tmp/zith-language-beta.vsix'
agent-run -t 30 -c 50 -- sha256sum /home/diogo/zith-lsp/build/zith-lsp
agent-run -t 30 -c 50 -- bash -lc 'set -o pipefail && unzip -p /tmp/zith-language-beta.vsix extension/server/zith-lsp | sha256sum'
```

**Expected output:** the LSP suite, extension tests, development host, and
packaged-VSIX host exit `0`. All five acceptance tasks pass on the packaged
artifact for every platform marked supported. Malformed import and control-flow
diagnostics state the problem and a valid correction, point to the relevant
source, and clear after a fix. Repeated playground/editor updates do not
duplicate identical diagnostics or retain stale ones. The hash of the LSP
binary embedded in the VSIX matches the reviewed build's SHA-256. Every
supported downloader mapping resolves to the expected published asset.

**Failure checks:**

- If a capability is advertised but has no passing server and host test,
  disable or defer that capability rather than promising it.
- If `vsce` is unavailable, stop this gate and ask the extension maintainer
  for the approved packaging command and environment. Do not install or
  download a different packager implicitly.
- If tests pass only against a development binary, the package gate has not
  passed. Rebuild and test the VSIX.
- If an asset is missing, published to a different repository, or built for a
  different target than the downloader requests, block that platform's support
  claim until the contract is fixed and tested.
- If a platform has only build or mocked-selection evidence, mark it
  unverified or unsupported and exclude it from the beta support claim.
- If a test fails because it needs a writable build tree or editor profile,
  run it in the owning repository's authorized environment. Do not work around
  repository permissions or treat an unrun test as a pass.

**Success criterion:** all five editor tasks pass against the exact candidate
VSIX, and every advertised capability has test evidence.

### 4. Bound And Verify The Beta Standard Library

**Window:** Weeks 2-4.

**Goal:** ship a small library surface whose names, maturity, and tested
behavior are clear to beta users.

**Actions:**

1. Compare the proposed beta module list with
   `docs/20-standard-library.md`, `docs/impl-status.md`, the release stdlib
   archive, and the files under `stdlib/`.
2. Classify every listed module as `std` stable, `soon` experimental, or `c`
   raw C binding. Publish no module as stable solely because it exists or
   type-checks.
3. Keep the initial beta surface to modules already needed by the verified
   examples and first-use tasks. Do not make `soon/json` or another stub part
   of the advertised beta API.
4. Run the module-isolation guard. For each beta-promised runtime behavior,
   identify a deterministic test on every beta execution target that supports
   it. Do not promote a module unless it meets the gate in
   `docs/plans/stdlib/soon-promotion.md`.
5. Document module purpose, import path, maturity, ownership/resource cleanup
   expectations, and one compiling example for every module in the beta list.

**Commands:**

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R 'test-stdlib-modules|test-stdlib-io|test-stdlib-parity|test-examples' --output-on-failure
```

**Expected output:** all selected tests pass. Every advertised module appears
in the reviewed beta module list with a passing type-check and runtime evidence
for each documented behavior.

**Failure checks:**

- If `test-stdlib-modules` finds a broken module, either fix it with a focused
  regression test or exclude it from the beta module list and public examples.
- If runtime behavior is tested only on one target, state that target
  limitation. Do not claim target-independent support.
- If a module has no stable contract tests, label it experimental or defer it.

**Success criterion:** zero failing tests for the advertised module set, with
no undocumented stable/experimental boundary.

### 5. Finish Documentation And Website Onboarding

**Window:** Weeks 3-5, after the beta surface and supported claims are frozen.

**Goal:** let a new reader understand what Zith-- is, install the beta, and
complete the verified first-use path without encountering contradictory
instructions.

**Actions:**

1. Reconcile the Zith-- pages in `/home/diogo/Zith-website` with the approved
   discrepancy table from Step 2. Preserve unrelated website changes.
2. Make the first-use path explicit: install, open a project, run `check`,
   understand a diagnostic, and run the verified example.
3. Link the introduction, specification, implementation status, VS Code
   installation instructions, standard-library module list, support matrix,
   and known limitations. State the expected familiarity with systems
   languages without implying that users need compiler implementation
   experience.
4. Separate Zith-- behavior from the Zith design specification on every page
   that currently mixes the names or contracts.
5. Verify the website playground or online editor against the current compiler
   and stdlib pack. If parity is not proven, label it experimental or omit it
   as a beta try-it path.
6. For every playground error fixture from Step 2, verify the displayed
   message, source location, and correction. Submit the same invalid source
   repeatedly and confirm the result does not accumulate identical
   diagnostics. Submit a corrected source and confirm stale diagnostics clear.
7. Coordinate any announcement article with
   `docs/plans/zith-minusminus-publicity.md`. Do not publish or push during
   this step.
8. Inspect every route enumerated by `tests/docs-routing.test.js` at viewport
   sizes 375x812 and 1440x900. Check horizontal overflow, primary navigation,
   page title, and browser-console errors.
9. Use keyboard navigation to reach the primary navigation, page content, and
   editor controls on each route that has those controls. Confirm visible
   focus and working links or controls.
10. Run the documented routing and editor tests explicitly. A successful site
   build or deployment workflow does not substitute for these tests.
11. Publish a limitations table that states each affected platform or feature,
    what was tested, what was not tested, the user impact, and any workaround.

**Commands:**

```bash
agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-editor.test.js
```

**Expected output:** the website build and both tests exit `0`. All linked
routes resolve. Each installation or playground instruction has been tried
against the candidate. Every enumerated route passes both viewport checks and
the keyboard checks.

**Failure checks:**

- If the website worktree contains unreviewed changes that affect the same
  pages, stop and coordinate with its owner. Do not overwrite, reset, or
  publish those changes.
- If a route, install step, command, or playground cannot be verified, remove
  it from the beta path or mark the limitation visibly.
- If any route has horizontal overflow, unusable primary navigation, missing
  visible keyboard focus, or a browser-console error, fix it and repeat the
  website checks before proceeding.
- If any critical/high discrepancy remains, do not announce the beta.

**Success criterion:** a reader can follow the published onboarding path from
the website to a passing local check/run without relying on an unverified
playground or undocumented setup.

### 6. Pilot, Cut, Exercise, And Approve The Release Candidate

**Window:** Weeks 5-6 for the existing user-facing work, as an estimate.
Continue beyond this window until the ABI and runtime identity gate also
passes.

**Goal:** approve a reproducible beta candidate whose compiler, LSP,
extension, website, and stdlib evidence all refers to the same reviewed state.

**Actions:**

1. Select reviewed commits for all repositories. Record their full commit IDs
   and the compiler version shown by the candidate.
2. Build the compiler and run the full CTest suite.
3. Run the example from the repository's documented entry point:
   `examples/loops-simple.zith`.
4. Run the LSP tests, VS Code extension-host test, packaged-VSIX test,
   website build/tests, and stdlib tests from Steps 3-5.
5. Repeat the ABI and runtime identity test matrix from Step 2 on every target
   in the final support matrix. Record the durable identity-state fixture,
   artifact-cache cleanup procedure, ABI version, and complete test output.
6. Run the five first-use tasks in a clean VS Code profile. Record pass/fail
   and attach logs or screenshots for failures.
7. Run a pilot with at least 3 and up to 5 first-time Zith-- users who match
   the profile in Scope And Baseline. Have each person complete the same five
   tasks without verbal help. Record task-by-task completion and the exact
   point where each person gets blocked.
8. Triage every pilot finding. Fix release-blocking issues and document lower
   priority limitations with impact and workaround. Re-run affected tests.
   The pilot is a mandatory gate, not optional feedback.
9. Fix only release-blocking defects in this window. Re-run every affected
   test and the full acceptance script after each fix.
10. Obtain explicit release approval before creating or publishing a public
   release. Do not push to the website's live branch or publish artifacts
   automatically. Publish the beta only after every gate, including the
   pilot, ABI, and runtime identity gates, passes. Use ABI stability as a
   public selling point only after its recorded evidence passes.

**Commands:**

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build --output-on-failure
agent-run -t 30 -c 50 -- /home/diogo/Zith/build/zithc check /home/diogo/Zith/examples/loops-simple.zith
agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-editor.test.js
```

Also repeat the LSP and extension commands in Step 3 and the stdlib command in
Step 4 against the selected candidate revisions.

**Expected output:** all required commands exit `0`. The full CTest suite
reports `100% tests passed`. All five first-use tasks pass. At least 3 pilot
participants complete the tasks and their findings are triaged. Every platform
in the public support matrix has end-to-end evidence. Revision IDs and test
evidence are recorded together.

**Failure checks:**

- If any required automated test is unrun, failed, or tied to a different
  revision, do not approve the candidate.
- If first-time users repeatedly fail the same task, treat it as a release
  blocker and fix the documentation or workflow before approval.
- If fewer than 3 pilot participants complete the tasks, do not publish the
  public beta. Recruit enough participants or obtain and record an explicit
  release exception.
- If an advertised platform has only a built artifact or mocked downloader
  test, remove it from the supported matrix or complete native end-to-end
  verification before approval.
- If the candidate has a crash, broken acceptance task, critical/high claim
  discrepancy, or missing/mismatched advertised artifact, block publication.
- If the ABI or runtime identity gate fails or lacks target-specific evidence,
  block publication even when every user-facing gate passes. Do not describe
  the ABI or runtime opaque tags as stable without the complete evidence.
- Keep lower-priority known limitations in the published limitations table,
  including their impact and workaround.
- If a fix changes compiler or LSP behavior, refresh the affected status,
  public claims, package, and regression tests before repeating acceptance.
- If the old 4-6 week user-facing estimate is exceeded, continue only with
  bounded work needed to pass the gates. Re-estimate the compiler ABI track
  after its design inventory. Defer publication rather than dropping a gate.

**Success criterion:** every acceptance gate passes on one recorded set of
reviewed revisions, including ABI and runtime identity on every supported
target, and release approval is explicit.

## Planning Estimate

The previous 4-6 week estimate covered only the user-facing beta tasks. It did
not include the newly mandatory ABI and runtime identity track, so it is not a
valid estimate for publication.

| Track | Planning window |
|---|---|
| Repository baselines and beta surface | Week 1, as previously planned. |
| Specification, implementation-status, and compiler audit | Weeks 1-2, as previously planned. |
| ABI and runtime identity design inventory | Complete by the end of Week 2, after the delta review. |
| ABI and runtime identity implementation and verification | Duration not yet estimated. Produce a revised range after the design inventory and before implementation begins. This is on the release critical path. |
| Editor, library, and website onboarding tasks | Existing windows remain planning estimates and may run in parallel with the compiler track. |
| Pilot and release candidate | Begin only after all other acceptance gates, including ABI and runtime identity, pass. |

Do not set a public beta date until the compiler-track estimate and the
cross-repository work estimate are recorded. Approval, defects, and tester
availability can change elapsed calendar time. If any gate takes longer than
estimated, continue with bounded fixes and verification. Defer the beta rather
than dropping a mandatory gate.

## Final Acceptance Record

Fill this table for the selected candidate before release approval:

| Repository | Execution-start HEAD and worktree state | Reviewed existing-work commit | Reviewed candidate commit |
|---|---|---|---|
| `/home/diogo/Zith` | 2026-11-24: `1994a9411476da763cd920980b589f95bf12cf5a`; branch `refactor/zithm-source-extension`, clean | Record if applicable | Record after review |
| `/home/diogo/zith-lsp` | 2026-11-24: `e55bfb78f794b6854fed580653f7c4faefd4d65f`; branch `main`, 11 commits ahead, modified and untracked files | 2026-11-24: `b40575f58b0b4c9040650f539ce8b8b666b23842` | Record after review |
| `/home/diogo/zith-extension` | 2026-11-24: `05b1696ea5153679478dcfafbbb173d7fad2a35e`; branch `main`, 6 commits ahead, modified and untracked files | 2026-11-24: `c4c62a61c3fd4b288811e8bcbd1a0145bebdd930` | Record after review |
| `/home/diogo/Zith-website` | 2026-11-24: `8946b5670795cded6c944c7c7944c02657ea45bd`; branch `gh-pages`, 1 commit ahead, modified and untracked files | 2026-11-24: `3d6d61f58b3531923fe232ca8f090d632c7cbba2` | Record after review |

Execution-start `git status --short --branch` was captured for all four
repositories on 2026-11-24. The compiler worktree was clean. The LSP,
extension, and website worktrees contained unreviewed local changes. The
captured revisions are historical starting points, not reviewed beta
candidates.

Captured worktree status:

```text
/home/diogo/Zith
## refactor/zithm-source-extension...origin/refactor/zithm-source-extension

/home/diogo/zith-lsp
## main...origin/main [ahead 11]
 M CMakeLists.txt
 M README.md
 M src/features/completion.cpp
 M src/features/signature.cpp
 M src/features/symbols.cpp
 M src/features/symbols.hpp
 M src/lsp/line-index.cpp
 M src/lsp/line-index.hpp
 M src/lsp/lsp-analysis-executor.cpp
 M src/lsp/lsp-analysis-executor.hpp
 M src/lsp/lsp-process-runner.cpp
 M src/lsp/lsp-process-runner.hpp
 M src/lsp/lsp-protocol.hpp
 M src/lsp/lsp-scheduler.cpp
 M src/lsp/lsp-scheduler.hpp
 M src/lsp/lsp-server.cpp
 M src/lsp/lsp-transport.cpp
 M src/lsp/lsp-transport.hpp
 M src/lsp/lsp-utils.hpp
 M tests/lsp_client.py
 M tests/test_files/visibility.zith
 M tests/test_lsp.py
?? CONTEXT-MAP.md
?? CONTEXT.md
?? docs/adr/
?? src/features/stdlib-symbol-index.cpp
?? src/features/stdlib-symbol-index.hpp
?? tests/__pycache__/

/home/diogo/zith-extension
## main...origin/main [ahead 6]
 M vs-code/README.md
 M vs-code/example.zith
?? .opencode/
?? vs-code/.zith-cache/
?? vs-code/cache/
?? vs-code/target/

/home/diogo/Zith-website
## gh-pages...origin/gh-pages [ahead 1]
 M .vscode/settings.json
 M README.md
 M js/playground-command-chain.js
 M notes/CONTEXT.md
?? css/glarea-alternative.css
?? helios/alternative.html
?? media/Glarea/
?? scripts/
?? worker/
```

Validation update, 2026-11-24:

- In `zith-lsp`, the stdlib auto-import index now enumerates the canonical
  `.zithm` source extension through the compiler's shared source-extension
  helper. The auto-import fixtures use `.zithm`, and the cross-module
  completion fixture now has a workspace root that resolves its sibling
  module. Existing local fixture content, including `pub struct Hidden`, was
  preserved.
- `agent-run -t 900 -c 0 -- cmake --build build -j4` in
  `/home/diogo/zith-lsp` exited 0. Ninja reported a truncated log and
  recovered, then rebuilt the existing tree.
- `agent-run -t 300 -c 0 -- python3 -m unittest discover -s tests -p 'test_lsp.py'`
  in `/home/diogo/zith-lsp` passed all 82 tests. The focused auto-import and
  semantic-completion classes also passed, 7 and 5 tests respectively.
- `agent-run -t 900 -c 0 -- ctest --test-dir build --output-on-failure -j4 -R 'test-frontend-context|test-frontend|test-stdlib-modules|test-frontend-modern-pipeline'`
  in `/home/diogo/Zith` passed all four selected compiler tests.
- LSP `git diff --check` and the compiler repository's `git diff --check`
  passed. Scoped `clang-format --dry-run --Werror` passed for the changed
  indexer lines. `cppcheck` exited 0 with warnings only in included compiler
  headers. `clang-tidy` reported numerous findings across included compiler
  headers, so it is not recorded as clean; no unrelated compiler headers were
  changed.
- `agent-run -t 300 -c 0 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && npm test'`
  passed. In the website repository,
  `agent-run -t 30 -c 0 -- node /home/diogo/Zith-website/tests/docs-editor.test.js`
  passed, while
  `agent-run -t 30 -c 0 -- node /home/diogo/Zith-website/tests/docs-routing.test.js`
  failed because `docs/D-overview.html` lacks the expected
  `data-docs-route="zith"` marker. The pending website changes do not modify
  the routing generator; leave that repository blocked pending triage.
- The LSP, extension, and website still contain unclassified local changes.
  No baseline commit was made. The issue-tracker update also remains
  unavailable because GitHub CLI authentication returned HTTP 401. Sequenta
  reports no ready task in the LSP module.

Baseline review and commit, 2026-11-24 (later in the session):

- Classified and committed the reviewed existing work in the three related
  repositories, each in its owning repository and separately from new work:
  - `zith-lsp` `b40575f58b0b4c9040650f539ce8b8b666b23842` (editor surface:
    completion, UTF-16 positions, frontend generation, transport, process
    streams, docs/ADRs, `.zithm` fixtures). `__pycache__/` was excluded and
    added to `.gitignore`.
  - `zith-extension` `c4c62a61c3fd4b288811e8bcbd1a0145bebdd930` (README,
    example, `.gitignore` for local build/cache artifacts).
  - `Zith-website` `3d6d61f58b3531923fe232ca8f090d632c7cbba2` (landing routes,
    async command chain, Glarea concept page, docs-editor Worker and wizard,
    CONTEXT/README). No secret values are present; `wrangler.toml` keeps the
    placeholder KV namespace id.
- All three related worktrees are now clean; `zith-lsp` and `zith-extension`
  are ahead of `origin/main`, and `Zith-website` is ahead of `origin/gh-pages`.
- The website routing failure is fixed at the source:
  `docs/pages/overview.md` now links the language routes through `route:` and
  the implementation status through `neutral:`. The generated
  `docs/D-overview.html` marker is produced by the build, not edited by hand.
- Additional evidence this session: the LSP suite passed 83 tests
  (previously 82) after adding a regression that proves the completion prefix
  honours UTF-16 columns; the extension `npm test` passed; the website
  `node --test` routing, editor, and command-chain tests passed 20/20 and
  `make build` exited 0. `git diff --check` passed in all four repositories.
- A proven beta blocker is recorded for the next step: the extension declares
  only the `.zith` language extension and activates on `onLanguage:zith`,
  while the compiler reads only `.zithm` sources. The extension must associate
  `.zithm` (and keep `.zith` only if intended for the flagship language) so a
  clean profile can activate on Zith-- files.

Step 1 is complete: the baseline revisions and intended existing changes are
recorded and committed, the website routing failure is resolved, and the
support scope (Zith-- only; macOS and Linux musl excluded) is unchanged.
Step 3 work on the extension/compiler extension mismatch is the next bounded
task.

Extension source-extension fix, 2026-11-24:

- The `.zith`/`.zithm` mismatch is fixed in `zith-extension` commit
  `eaf7925`: the manifest associates `.zithm` and adds `Zith--`/`zith--`
  aliases, `zith.new` opens `src/main.zithm`, the example and completion
  fixture are renamed, and a package-compatibility test asserts the
  association. `npm test` passed (unit 9, project 5, integration 21 with the
  new association test). The compiler scaffolds `src/main.zithm` with
  `zithc create`, so the opened path matches.
- The Linux-only extension-host harness (`node test/integration/run-host.js`)
  did not complete in this environment: the wrapper hands off to an already
  running code-oss instance and times out even with an isolated profile. This
  is the known harness limitation recorded in Current CI Evidence Gaps, not a
  regression from this change.

LSP review fixes, 2026-11-24:

- The same `.zith`/`.zithm` mismatch was still present inside `zith-lsp`: the
  watched-files glob and handler, the workspace-scan filter, the project
  scaffold, and the completion module detail all keyed on the reserved
  `.zith`. Commits `9093bdc` and `c46c10a` route every decision through
  `support/source-extension.hpp`, register a `**/*.zithm` watcher, and rename
  the test fixtures to `.zithm`.
- Commit `f019982` removes the process-global stdlib symbol cache that
  violated ADR 0005. The index is now workspace-owned by `AnalysisExecutor`
  and dropped when a watched stdlib source changes. A regression appends a
  public symbol to a stdlib module and asserts it becomes visible; the test
  fails when the invalidation is removed.
- Commits `33397b0e` (compiler) and `db043f9` (LSP) promote the compiler's
  `PerModuleSema::fieldVisible` to public and consume it from
  `membersForType`, so the IDE no longer reimplements the field-visibility
  rule (ADR 0003).
- Commits `940d78c` and `56df0b8` share a single `DeclKind` to
  `CompletionItemKind` mapping between the local-declaration and stdlib-symbol
  completion paths.
- Commits `6899b1c` and `b6b10cba` correct the documented completion trigger
  characters to include space and slash.
- Evidence: `cmake --build build -j4` and the LSP suite passed 85 tests in
  `/home/diogo/zith-lsp`; the compiler build passed and
  `ctest -R '^(test-ide-api|test-frontend|test-cache|test-hir-lower-modern|test-codegen)$'`
  passed 5/5 in `/home/diogo/Zith`; `git diff --check` passed in both.

Two-axis review of the LSP review fixes, 2026-11-24:

- Standards axis: no hard violations. Actionable finding applied in commit
  `eb54fb9`: the watched-files glob literal is now derived from
  `kZithmSourceExtension` instead of duplicating `"**/*.zithm"`. The
  duplicated `DeclKind` mapping and the field-visibility reimplementation were
  confirmed removed.
- Spec axis: found the stdlib-change check was a raw string prefix match.
  Commit `eb54fb9` replaces it with a normalized, component-aware
  `pathIsInside` check and registers a second watched-files glob rooted at the
  stdlib path, since the workspace-relative glob never reaches a stdlib
  outside the workspace. A regression asserts the stdlib watcher is
  registered.
- Evidence after the fixes: LSP suite passed 86 tests; the compiler full CTest
  suite passed 51/51.
- Still open for Step 3, recorded here so it is not mistaken for done: the
  `isIncomplete` partial-edit completion path is deferred (documented in
  `zith-lsp/docs/completion-recommendation-plan.md`); the LSP/VS Code handoff
  document still needs the tested command names, settings, and known
  limitations beyond the corrected trigger characters; and the packaged/installed
  server SHA-256 values (Step 3 Action 7) plus the extension-host and
  five-first-use-task evidence (Step 3 Actions 4-6) remain to be collected on a
  display-capable host.

Attach or link the following evidence in the release review:

- Candidate version and full commit IDs for the compiler, LSP, extension, and
  website.
- Supported operating systems and exact installation artifacts.
- For each platform and architecture, whether its artifact was built,
  selection-tested, native-smoke-tested, and end-to-end tested.
- The release repository and exact asset name selected by each extension
  downloader mapping.
- The frozen beta standard-library module list with maturity labels.
- The completed spec/status/compiler discrepancy table.
- The finalized ABI delta and ADR, target-specific ABI version, emitted-symbol
  regression results, structured cache metadata round-trip results, and
  runtime opaque-tag evidence for cold/warm builds, module-order permutations,
  and module addition/removal with durable identity state preserved.
- Build, CTest, LSP, extension-host, VSIX, website, and stdlib test results.
- Diagnostic fixture results for malformed imports and unbraced control-flow
  bodies, including message quality, source spans, and correction suggestions.
- Playground evidence that repeated diagnostics are not duplicated and stale
  diagnostics clear after a successful compile.
- Five-task acceptance results and pilot results for at least 3 participants,
  including triaged findings.
- Known limitations with impact and workarounds, unresolved blockers, and
  explicit release approval.

Step 3 handoff and packaging evidence, 2026-11-24:

- The LSP/VS Code handoff was refreshed in `Zith` commit `4ef28125`
  (`docs/vscode-extension-lsp-api.md`). Section 4 now lists what extension
  `0.4.0` implements and tests plus what remains (frontend initialization
  options, `zith/frontendStatus`, `zith/metrics`, explicit cancellation).
  Section 5 replaces the stale implementation checklist with the tested
  command names, the extension settings and which of them `extension.js`
  reads, the test evidence, and the known limitations (macOS and Linux musl
  out of scope, extension-host harness limitation, deferred `isIncomplete`
  path, `workspace/symbol` shape, and the unread `zith.enableHighlighting`).
- `zith-lsp` commit `3221ccc` corrects `docs/ide-extension-api.md`, which
  claimed `workspace/symbol` had no background index. It is served from open
  documents scanned synchronously plus the workspace symbol index built after
  `initialized`.
- Step 3 Action 7 (server SHA-256), recorded 2026-11-24:
  - reviewed LSP build `zith-lsp/build/zith-lsp` at `zith-lsp` HEAD
    `3221ccc348302ce108c5b9182ce39de5e0224cf1` and `Zith` HEAD
    `66ae5846b4c06d814575dca32eac70895358a792` (the LSP links the local
    compiler source):
    `3c58a91c252151e3d18b79b2c59a688e5b20c06e6b1fd1840dc94e48e0f9a8ec`.
  - installed extension server binary
    `zith-extension/vs-code/server/zith-lsp`:
    `3c58a91c252151e3d18b79b2c59a688e5b20c06e6b1fd1840dc94e48e0f9a8ec`.
  - packaged VSIX `/tmp/zith-language-beta.vsix`, entry
    `extension/server/zith-lsp`:
    `3c58a91c252151e3d18b79b2c59a688e5b20c06e6b1fd1840dc94e48e0f9a8ec`.
  All three match, so the packaged server is the reviewed build. The previous
  packaged `zith-lsp` was the stale 2026-11-08 binary, now replaced.
- The earlier recorded hash `0cf2fada...` was superseded by the frontend fix in
  `Zith` commit `1942febf`. Any candidate artifact must be re-packaged from the
  build whose hash is recorded here.
- Still open for Step 3: the packaged VSIX bundles only 9 stdlib files under
  `server/stdlib`, while the compiler ships 34 under `stdlib/`. The downloader
  fetches the full tree from `GalaxyHaze/Zith-Lang`, so a clean install gets
  the full stdlib, but the checked-in VSIX payload is incomplete. Step 3
  Actions 4-6 (five first-use tasks, duplicate/stale diagnostics, VSIX host
  test) still need a display-capable host. The Linux extension-host harness
  does not complete here.
- Evidence: `cmake --build /home/diogo/zith-lsp/build -j4` exited 0, the LSP
  suite passed 86 tests, and `npm test` in `zith-extension/vs-code` passed.
  `vsce package` produced a 350-file VSIX and `git diff --check` passed in
  both repositories.

Step 2 malformed-source fixtures, 2026-11-24:

- Action 5 found a real gap for the `import / from path{...}` form. Before the
  fix, `zithc check` emitted a generic `E0001 expected an import path` followed
  by a cascading `E1006 could not find module`. Commit `1942febf` makes the
  frontend look past path separators between `import` and `from`, emits the
  dedicated `E1013` message that names both valid forms (`from path` imports
  symbols, `import path` imports a namespace), and marks the recovered
  declaration as an error so resolution does not add the cascade. The syntax
  stays invalid.
- `import from path{...}` already reached `E1013` and is unchanged. Both forms
  now produce exactly one diagnostic with the actionable message and span.
- Regressions added: `test-frontend.cpp` asserts one `E1013` for the slashed
  form, a retained following declaration, and that a genuine path segment
  named `from` (`import graphics/from extra`) is not misread as the malformed
  prefix. `test-wasm-playground.cpp` submits both malformed prefixes through
  the playground and asserts one structured `E1013`, no `expected an import
  path` cascade, and a single host-stderr write.
- Action 6 is already satisfied: `zithc check` on `if (true) 42;` reports
  `E1012 expected '{' before if body; write 'if (condition) { ... }'` with the
  correction note, and the playground asserts one structured `E1012`.
- Evidence: `cmake --build build -j4` exited 0, the full CTest suite passed
  51/51, `./build/test-frontend` passed 467/467, `./build/test-wasm-playground`
  passed 67/67, and `./build/zithc check examples/loops-simple.zithm` passed.

Step 2 discrepancy table and website source-file recheck, 2026-11-24:

- Action 3 deliverable created: `docs/spec-status-discrepancy-table.md`
  records one row per `docs/impl-status.md` status row plus every public-claim
  finding, each with the spec section, status entry, compiler evidence (a test
  id or a `zithc` probe result), discrepancy, and resolution. Every status row
  was rechecked against the compiler at `Zith` HEAD `29444d4e` and none
  contradicts the compiler.
- Actions 1, 3, 4, 9, 10: the table re-audits the four items in
  `docs/review-website-inconsistencies-2026-11-02.md` and adds two more
  website discrepancies found by rechecking source-file references. All six
  are fixed and committed.
- New public-claim discrepancy (`.zithm` vs `.zith`): `Zith-website`
  `zith--/reference.md` listed platform-specific import variants as
  `foo.<arch>.<os>.zith`, and `zith--/guide/first-program.md` plus the CLI
  pages used `src/main.zith` / `repro.zith`. The compiler scaffolds
  `src/main.zithm` (`src/cli/cmd/project.cpp`) and resolves module candidates
  only for `.zithm` (`src/session/frontend-module-analysis.cpp`). Fixed in
  `Zith-website` commit `3097311` (nine references across eight pages).
- Evidence: `ctest --test-dir build --output-on-failure -j4` passed 51/51,
  `ctest -R 'docs-consistency|concepts-consistency|deltas-consistency'` passed
  3/3, `zithc check` passed 50 of 55 `examples/*.zithm` (the five failures are
  the exploratory `test-import-*.zithm` probes over the experimental `soon/`
  tree, not the `test-examples` manifest), `zithc create` wrote
  `src/main.zithm`, and `git diff --check` passed in both repositories.

Step 4 beta module list and `std`/`soon` drift, 2026-11-24:

- Action 1 recheck found a real drift. Commit `51bc0487` ("refactor(stdlib):
  remove legacy facades", 2026-11-01) deliberately deleted the `std/`
  re-export facades for `result`, `panic`, `alloc`, `new`, `counter`, `math`,
  `collections/{hash_map,hash_map_u64,dyn_array}`, and
  `memory/{in-place,new,allocators/*}`. The modules now ship only under
  `soon/`. The commit `650c0acf` migrated the C++ tests to the `soon/`
  re-export path. Probes against `build/zithc` confirm every `std/...` path
  above now fails with `E1006 could not find module`, while the `soon/...`
  paths resolve.
- Actions 2 and 3: the authoritative docs still advertised the removed `std/`
  paths. `docs/20-standard-library.md` (module list plus the `std/alloc`,
  `std/collections/*`, `std/result`, and `std/panic` sections),
  `docs/impl-status.md` (Stdlib allocation/collections/error-handling rows),
  `docs/adr/0010`, `docs/adr/0020`, `docs/adr/0021`, the `memory/*` notes,
  `docs/research/stdlib-references-{zig,go}.md`, two module comments, and one
  `test-codegen` message were corrected to the `soon/...` paths. The `std/`
  surface is now only `std/io/console`, `std/io/format`, `std/scope`,
  `std/test`, and the `std/memory` facade. This follows the roadmap rule to
  keep the beta surface small and not promote `soon/` modules to enlarge it.
  The deleted facades are not restored.
- Action 5 deliverable created: `docs/beta-module-list.md` classifies every
  `stdlib/**.zithm` module as `std` stable, `c` raw, or `soon` experimental,
  and gives each stable module its purpose, import path, cleanup expectations,
  test evidence, and one compiling example. `soon/json` is explicitly excluded
  from the beta API.
- Action 4: `test-stdlib-modules` type-checks every `stdlib/**.zithm` in
  isolation. The stale `hash_map_u64` example in `docs/20-standard-library.md`
  was corrected to the real `hm.create(heap)` constructor (the old struct
  literal omitted the required `allocator` field).
- Evidence: `cmake --build build -j4` exited 0; `ctest --test-dir build -R
  'test-stdlib-modules|test-stdlib-io|test-stdlib-parity|test-examples'`
  passed 4 of 4; every module-list snippet passed `zithc --include stdlib
  check`; `git diff --check` passed.

Step 5 website onboarding, 2026-11-24:

- Action 1: the Zith-- website pages were already reconciled with the Step 2
  discrepancy table in commits `92161c1` and `3097311`. Rechecked this turn
  and found no remaining critical or high public-claim discrepancy.
- Action 2: the first-use path is now explicit. The new
  `docs/pages/getting-started/beta-support.md` states the supported route
  (install, `zithc create`, `zithc check`, correct a diagnostic, `zithc run`),
  and `zith--/guide/first-program.md` links the support matrix, the editor
  setup page, and the standard-library reference.
- Action 3: added `docs/pages/getting-started/editor-setup.md` with the VS Code
  install path (download `zith-lsp`, install the `.vsix` or build it with
  `npx @vscode/vsce package`, open a project, check and clear a diagnostic).
  The page is scoped to the verified Linux x86-64 platform. The standard
  library, support matrix, and known limitations are now linked from the
  Zith-- onboarding pages.
- Action 4: `zith--/overview.md` already separates Zith-- behavior from the
  Zith design specification. No page mixes the two contracts.
- Action 5: the playground is labelled experimental on `playground/index.html`
  and in the limitations table, because parity with the current compiler and
  stdlib pack is not proven.
- Action 6: the compiler-side `test-wasm-playground.cpp` already covers the
  error-fixture behavior: repeated invalid source keeps its status and does not
  accumulate diagnostics, and a later compile clears stale entries.
- Actions 8 and 9 (viewport and keyboard checks at 375x812 and 1440x900): not
  run in this pass because the environment had no headless browser at the time.
  Both were run in the follow-up pass recorded below (Playwright Chromium
  became available); see "Step 5 Actions 8 and 9" in the log.
- Action 10: `node tests/docs-routing.test.js` passed 7 of 7 and
  `node tests/docs-editor.test.js` passed 4 of 4.
- Action 11: the limitations table ships in
  `docs/pages/getting-started/beta-support.md`, one row per affected platform
  or feature, with what was tested, what was not, the user impact, and the
  workaround.
- Website commits: `4d5892b` (support matrix and install fix), `518990e`
  (editor setup page and cross-links), `81215f5` (playground experimental
  label).
- Evidence: `make site` built 145 documentation fragments and exited 0;
  `make docs-check` reported the fragments current; `npx @vscode/vsce package`
  produced `zith-language-0.4.0.vsix` from the extension source; the install
  page's broken `git chttps://...` command was corrected to `git clone`.
- Open at the time: Step 5 Actions 8 and 9 needed a browser. Resolved in the
  follow-up pass recorded below.

Step 3 and Step 6 editor test runs, 2026-11-24:

- Step 6 Action 4, LSP: `python3 -m unittest discover -s tests -p test_lsp.py`
  in `zith-lsp` ran 86 tests and passed. The suite covers the `zith.*`
  command set, the `zith/codegenStatus` warning, the `zith.run`/`zith.stop`
  flow, and `zith/metrics`.
- Step 6 Action 4, extension unit tests: `node test/unit/test-download.js`
  passed 9 of 9 and `node test/unit/test-project.js` passed 5 of 5. The
  downloader test verifies asset-name selection per OS and architecture. The
  project test verifies the `zith.new` rootUri normalization and TextEdit
  helpers.
- The extension integration host (`node test/integration/run-host.js`) and the
  packaged-VSIX host (`run-vsix-host.js`) still cannot complete here. They
  require a VS Code window and the environment has no display, no Xvfb, and no
  headless browser. Step 3 Actions 4-6 stay open for that reason; Step 5
  Actions 8 and 9 were completed in the pass below.
- The recorded LSP test count in `docs/vscode-extension-lsp-api.md` section 5.3
  matches this run (86 tests).

Step 5 Actions 8 and 9, 2026-11-24 (headless-browser pass):

- A headless browser became available for this pass. Playwright Chromium
  (`playwright-core` 1.48.0, Chromium revision 1140) runs against `_site/`
  served over HTTP on the loopback interface.
- Action 8: every route enumerated by `tests/docs-routing.test.js` was
  inspected at 375x812 and 1440x900. The checker asserts horizontal overflow
  (`scrollWidth - clientWidth <= 1`), presence of the primary navigation,
  a non-empty page title, non-empty content, and no browser-console or
  page errors. Result: 36 of 36 route/viewport combinations passed.
- Real defects found and fixed by Action 8:
  - Documentation tables rendered as a bare `<table>` pushed the page wider
    than the viewport at 375px (`reference/D-implementation-status.html`
    overflowed 629px and `getting-started/D-beta-support.html` overflowed
    169px). Both renderers now wrap tables in `<div class="docs-table-wrap">`
    and `docs.css` clips them with `overflow-x: auto`, so the table scrolls
    instead of the page.
  - The playground WASM module failed to instantiate on the main thread
    because its linear memory exceeds the 8 MiB synchronous limit
    (`WebAssembly.Instance is disallowed on the main thread, if the buffer
    size is larger than 8MB`). The `instantiateWasm` callback now uses the
    async `WebAssembly.instantiate` API and the runtime status reaches
    "Ready".
- Action 9: keyboard traversal was checked on every route at 1440x900. The
  checker tabs through the page and asserts that the primary navigation, the
  page content, and the editor controls (on the playground) are all reachable
  with visible focus, and that activating a primary navigation link with Enter
  navigates. Result: 18 of 18 routes passed.
- Real defect found and fixed by Action 9: `#source-code` and `#terminal-input`
  cleared the default outline with ID specificity, which also overrode the
  global `:focus-visible` rule, so keyboard focus was invisible on both
  controls. Each now carries its own `:focus-visible` outline.
- Action 5/6 playground parity, verified end to end in the same browser:
  the default program prints `Hello, World!`. A malformed program prints one
  diagnostic and does not accumulate it across repeated runs. A corrected
  program clears the stale diagnostic and runs to exit code 0. The duplicate
  diagnostic the compiler produced by re-printing `readLastError()` on top of
  the streamed `zith.host_write` output was removed.
- Action 10 rerun after the fixes: `node tests/docs-routing.test.js` passed
  7 of 7, `node tests/docs-editor.test.js` passed 4 of 4, and
  `node tests/playground-command-chain.test.js` passed 9 of 9.
- Website commits: `573b747` (table wrapper), `bdbd054` (async WASM load,
  refreshed default, single diagnostic), `1fb661e` (visible focus ring).
- Evidence: `make site` built 145 documentation fragments and exited 0. The
  viewport checker reported 36/36 and the keyboard checker 18/18. The
  playground diagnostic probe showed one diagnostic per run and a clean
  corrected run.
- Still open: Step 3 Actions 4-6 (VS Code extension-host and packaged-VSIX
  host tests) need a running code-oss window and remain unverified here.
