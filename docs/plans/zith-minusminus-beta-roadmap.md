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
  across multiple architectures. Installer smoke workflows cover Ubuntu,
  macOS, Windows, and a WASM bundle, but do not demonstrate native Windows
  ARM64 execution. See `.github/workflows/build-artifact.yml` and
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
  that this exact asset is published in that repository. See
  `/home/diogo/zith-extension/shared/scripts/download-lsp.js` and the
  LSP/compiler release workflows. Verify the complete repository, asset-name,
  and architecture contract before including macOS in the supported beta
  matrix.

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

**Goal:** define the smallest useful beta experience and record the exact
starting state of every repository.

**Actions:**

1. Record `git status --short --branch` and `git rev-parse HEAD` in
   `/home/diogo/Zith`, `/home/diogo/zith-lsp`,
   `/home/diogo/zith-extension`, and `/home/diogo/Zith-website`.
2. In each related repository, inspect tracked and untracked changes, confirm
   which existing changes are intended, run the relevant focused tests, and
   commit the reviewed baseline work before making new changes. Keep commits
   separate by repository. If a change is unexplained or a focused test fails,
   stop work in that repository and record the blocker. Do not stage generated
   files or unrelated changes.
3. Define the user profile as a person new to Zith-- who can use a terminal,
   build tools, and basic systems-language concepts, but does not need compiler
   implementation experience.
4. Write down the proposed compiler targets, installation routes, LSP assets,
   extension download sources, and standard-library modules. For every
   platform and architecture, distinguish artifact built, selection mocked,
   native smoke-tested, and end-to-end tested. Claim support only after the
   platform-support gate passes.
5. Use five first-use tasks as the acceptance script: install the extension,
   open a project, observe an actionable error diagnostic and clear it by
   correcting the source, get one advertised editor result such as completion
   or hover, and successfully check or run the verified example.
6. Keep unrelated language features, broad standard-library growth, compiler
   architecture changes outside the ABI and identity track, and a standalone
   IDE out of scope. The ABI and identity track is the explicit compiler-work
   exception and is mandatory for publication.

**Commands:**

```bash
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith status --short --branch
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith rev-parse HEAD
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-lsp diff --check
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-lsp diff --cached --check
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-lsp status --short --branch
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-lsp rev-parse HEAD
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-extension diff --check
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-extension diff --cached --check
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-extension status --short --branch
agent-run -t 30 -c 50 -- git -C /home/diogo/zith-extension rev-parse HEAD
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website diff --check
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website diff --cached --check
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website status --short --branch
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website rev-parse HEAD
agent-run -t 900 -c 0 -- cmake --build /home/diogo/zith-lsp/build -j4
agent-run -t 900 -c 0 -- bash -lc 'cd /home/diogo/zith-lsp && python3 -m unittest discover -s tests -p "test_lsp.py"'
agent-run -t 300 -c 50 -- bash -lc 'cd /home/diogo/zith-extension/vs-code && npm test'
agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-editor.test.js
```

**Checks and evidence:**

- All four repository revisions and dirty-file lists are recorded in the
  execution table under Final Acceptance Record.
- Existing intended changes in the three related repositories have been
  reviewed, the commands above pass, and the changes have been committed
  separately before new work begins.
- The beta support matrix names each supported platform, architecture,
  artifact, installation route, and end-to-end test evidence.
- The acceptance script has five numbered tasks, expected results, and a
  pass/fail field. It also records the assumed user profile.

**Failure checks:**

- If a related repository has changes that are unexplained or failing focused
  tests, do not begin new work or package artifacts from it.
- If a platform lacks native end-to-end evidence, mark it unverified or
  unsupported. Do not infer support from an artifact build or mocked test.
- If any proposed work requires a language redesign or a new IDE, defer it
  beyond beta.

**Success criterion:** all baseline revisions and intended existing changes
are recorded, the related baseline changes are committed, and the user profile,
support matrix, and five-task acceptance script are reviewed.

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
   `zith-lsp-macos-universal` asset mismatch identified above must be resolved
   by correcting the download or publication contract, or by removing that
   combination from the supported beta matrix.
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
| `/home/diogo/Zith` | Record with Step 1 commands | Record if applicable | Record after review |
| `/home/diogo/zith-lsp` | Record with Step 1 commands | Record before new work | Record after review |
| `/home/diogo/zith-extension` | Record with Step 1 commands | Record before new work | Record after review |
| `/home/diogo/Zith-website` | Record with Step 1 commands | Record before new work | Record after review |

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
