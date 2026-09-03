# Standalone C Toolchain Plan

Objective: move Zith native builds from host-provided TinyCC/libclang/toolchain assumptions to a standalone `zithc` distribution with embedded LLD, a clean toolchain backend interface, a Zith-owned C binder, validated C ABI, and a WASM header bundle, without changing the Zith language surface.

Preconditions:

- Working directory: `/home/diogo/Zith`.
- Existing build: `/home/diogo/Zith/build` configured with Ninja.
- Host toolchain available: `clang++`, CMake 3.20+, Ninja or Make, LLVM/Clang 21 libraries, `lld` headers/libraries if present, `libclang`.
- Do NOT run `git reset --hard`, `git checkout --`, or delete unrelated user changes.
- Do NOT modify `src/wasm/`, `src/cli/` feature behavior, or the Zith language spec unless a step explicitly says so.
- Do NOT remove `libclang` code in the same commit that introduces the Zith-owned binder; use feature gates and keep one fallback at a time.

Files expected to change:

- `/home/diogo/Zith/CMakeLists.txt`
- `/home/diogo/Zith/src/cc/driver.hpp`
- `/home/diogo/Zith/src/cc/driver.cpp`
- `/home/diogo/Zith/src/cc/toolchain.hpp` (new)
- `/home/diogo/Zith/src/cc/toolchain.cpp` (new)
- `/home/diogo/Zith/src/cinterop/c-header.hpp`
- `/home/diogo/Zith/src/cinterop/c-header.cpp`
- `/home/diogo/Zith/src/cinterop/c-binder.hpp` (new)
- `/home/diogo/Zith/src/cinterop/c-binder.cpp` (new)
- `/home/diogo/Zith/src/wasm/headers/` (new, only after WASM phase)
- `/home/diogo/Zith/tests/test-c-compile.cpp`
- `/home/diogo/Zith/tests/test-cinterop.cpp`
- `/home/diogo/Zith/tests/test-c-binder.cpp` (new)
- `/home/diogo/Zith/docs/18-c-interop.md`
- `/home/diogo/Zith/docs/impl-status.md`
- `/home/diogo/Zith/docs/adr/0006-standalone-native-toolchain-bundle.md`
- `/home/diogo/Zith/docs/adr/0007-embedded-lld-in-process.md`
- `/home/diogo/Zith/docs/adr/0008-zith-owned-c-binder-and-tiny-backend-roadmap.md`
- `/home/diogo/Zith/docs/adr/0009-validated-c-abi-surface.md`

The ADRs already exist and must be updated only when accepted status changes or consequences are disproven.

## Step 1 - Add the Toolchain Backend interface

Goal: `/home/diogo/Zith/src/cc/toolchain.hpp` defines the backend capability contract used by compile/link/header/system-include paths, without changing runtime behavior.

Sub-steps:

1. Create `/home/diogo/Zith/src/cc/toolchain.hpp`.
2. Define `enum class ToolchainCapability : uint32_t { NativeCompile, NativeLink, HeaderImport, SystemIncludeDiscovery, WasmHeaderBundle }`.
3. Define `struct ToolchainBackend { std::string name; uint32_t capabilities; bool has(ToolchainCapability) const; };`.
4. Define `class ToolchainRegistry` with `registerBackend`, `findDefaultFor(capability)`, and `describe()`.
5. Create `/home/diogo/Zith/src/cc/toolchain.cpp`.
6. In `toolchain.cpp`, implement `ToolchainRegistry` with a deterministic default order: Zith-owned backend first when exposed, embedded LLD second, bundled Clang driver third, libclang fourth.
7. Do not call the registry from `compileCSource()` yet.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build
    cmake --build /home/diogo/Zith/build -j4
    ./build/zithc info

Expected output:

- Build exits 0.
- `zithc info` still prints the previous output, unchanged.

Failure checks:

- If CMake fails with `unknown cmake generator`: use `cmake -S /home/diogo/Zith -B /home/diogo/Zith/build -G Ninja`.
- If the build fails with missing symbols in `cc::ToolchainRegistry`: open `/home/diogo/Zith/CMakeLists.txt`, confirm `file(GLOB_RECURSE ZITH_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")` includes `src/cc/toolchain.cpp`; if the file still is not in the glob, rerun `cmake -S /home/diogo/Zith -B /home/diogo/Zith/build`.
- If `zithc info` output changed: stop and report the diff verbatim.

Success criteria:

- `ToolchainBackend` and `ToolchainRegistry` compile with no warnings.
- `zithc info` output is identical before and after this step.

## Step 2 - Wire compile backend selection through the registry

Goal: `src/cc/driver.cpp` asks `ToolchainRegistry` which C compile backend to use and still produces the same objects as today.

Sub-steps:

1. Open `/home/diogo/Zith/src/cc/driver.hpp`.
2. Change `CCompileBackend` to add `BundledClang` and `ZithOwned` values after `LibTcc`.
3. Open `/home/diogo/Zith/src/cc/driver.cpp`.
4. Replace the `#if ZITH_C_COMPILE_AVAILABLE` direct call in `compileCSource()` with a call to `ToolchainRegistry::findDefaultFor(ToolchainCapability::NativeCompile)`.
5. Keep `compileWithTcc`, `compileWithExternalCc`, and the existing fallback code inside helper functions; do not delete them.
6. When the selected backend is `LibTcc`, call the existing `compileWithEmbeddedBackend`.
7. When the selected backend is `BundledClang`, call a new `compileWithBundledClang` helper using `compilerDriverPath()`.
8. When the selected backend is neither, return `CCompileDiagnostic::CCompileDisabled` with a clear message.
9. Update `zithc info` to print the selected backend names from `ToolchainRegistry::describe()`.

Commands:

    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build --output-on-failure

Expected output:

- `100% tests passed, 0 tests failed out of <N>` or the same number of failures as the baseline before this session.
- `test-c-compile` still passes or fails only for the pre-existing known set documented before this session.

Failure checks:

- If test-c-compile fails with `backend` enum mismatch: update any switch over `CCompileBackend` in `src/cc/` and `tests/`.
- If old libtcc tests stop passing and the baseline already ran with `ZITH_C_COMPILE_AVAILABLE=0`, compare `build/compile_commands.json` to confirm the compile definition did not change.
- If `zithc info` output changes for unrelated resource dirs, stop and report the exact output.

Success criteria:

- `zithc info` lists every available backend and the selected default for compile.
- Existing `test-c-compile` passes where it did before.

## Step 3 - Embed LLD in-process by default

Goal: native ELF linking uses `lld::lldMain` through `src/cc/driver.cpp` whenever the build has LLD, and the external driver is no longer the default.

Sub-steps:

1. Open `/home/diogo/Zith/src/cc/driver.cpp`.
2. In `linkNative()`, keep the existing `#if ZITH_LLD_AVAILABLE` branch but make `linkWithLld` the default for every non-WASM triple whose object format is ELF.
3. Keep `linkWithExternalCc` available behind an explicit `ToolchainBackend::ExternalDriver` capability, not as silent fallback.
4. Extend `toolchain.hpp` with `enum class LinkBackendProfile { Elf, Coff, MachO, Wasm }`.
5. In `toolchain.cpp`, register `EmbeddedLld` with `NativeLink` capability.
6. Add a CMake option `ZITH_EMBED_LLD=ON` defaulting to ON when LLD is found.
7. In `CMakeLists.txt`, when `ZITH_EMBED_LLD` is ON and `ZITH_LLD_AVAILABLE`, link `lldELF`/`lldCommon` to `zithcLib` as today.
8. Ensure CMake does not fall back to the external driver silently when `ZITH_LLD_AVAILABLE=OFF`; the CLI should report the unavailable capability.
9. Update `zithc info` to print `LLD: embedded/ELF` or `LLD: unavailable`, plus the fallback policy.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build -DZITH_EMBED_LLD=ON
    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build --output-on-failure
    ./build/zithc info

Expected output:

- `zithc info` prints `LLD: embedded/ELF`.
- Link tests pass for native ELF builds.

Failure checks:

- If `lld::lldMain` fails to link on this machine, first check the `ZITH_LLD_INCLUDE_DIR`/`ZITH_LLD_ELF_LIBRARY`/`ZITH_LLD_COMMON_LIBRARY` cache values with `rg "ZITH_LLD" /home/diogo/Zith/build/CMakeCache.txt`; reconfigure with `-DZITH_LLD_AVAILABLE=OFF` only after recording the exact build error.
- If Windows/Mac test artifacts appear with `ELF` expectations, do not add platform-specific tests in this step; keep them for Step 4.
- If old external-link tests expected command output from `/usr/bin/cc`, update those tests only if they assert a backend default that this plan intentionally changes.

Success criteria:

- A native ELF `zithc build` proceeds through embedded LLD without invoking a user linker.
- `zithc info` makes the LLD state explicit.

## Step 4 - Add native link profiles for COFF and Mach-O

Goal: `ToolchainRegistry` can select ELF, COFF, or Mach-O LLD drivers based on the target triple, with platform tests behind CI gates.

Sub-steps:

1. Open `/home/diogo/Zith/CMakeLists.txt`.
2. Extend LLD detection to look for `lldCOFF` and `lldMachO` libraries alongside `lldELF` when they exist.
3. Define CMake cache strings `ZITH_LLD_COFF_LIBRARY` and `ZITH_LLD_MACHO_LIBRARY`, leaving them empty when unavailable.
4. In `src/cc/toolchain.hpp`, add `enum class LinkBackendProfile { Elf, Coff, MachO, Wasm }`.
5. In `src/cc/toolchain.cpp`, map a target triple to `LinkBackendProfile` using `llvm::Triple`.
6. In `src/cc/driver.cpp`, pass the profile to `linkWithLld` and select the correct `lld::DriverDef`.
7. Add tests in `tests/test-c-compile.cpp` for helpers that map triples like `x86_64-pc-windows-msvc` to `Coff` and `aarch64-apple-darwin` to `MachO`.
8. Do not claim runtime link success for COFF/Mach-O until a real CI host exercises those binaries.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build -DZITH_EMBED_LLD=ON
    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build --output-on-failure

Expected output:

- The new triple-mapping tests pass.
- ELF linking still passes.

Failure checks:

- If the host lacks lldCOFF/lldMachO libraries, expect `ZITH_LLD_COFF_LIBRARY`/`ZITH_LLD_MACHO_LIBRARY` to be empty and do not force a configure failure.
- If a test tries to link a COFF object on Linux, mark it `REQUIRES_WINDOWS` rather than attempting host execution.

Success criteria:

- The linker abstraction can select the right LLD driver from a triple.
- Native ELF continues to build/link.

## Step 5 - Add system include discovery behind the backend interface

Goal: `cinterop::systemIncludeDirs` is no longer coupled to libclang-only probing; it delegates through `ToolchainRegistry`.

Sub-steps:

1. In `/home/diogo/Zith/src/cc/toolchain.hpp`, add `virtual std::vector<std::string> systemIncludeDirs(targetTriple, sysroot) const`.
2. In `/home/diogo/Zith/src/cinterop/c-header.cpp`, replace calls to `probeSystemIncludeDirs` inside `systemIncludeDirs()` with a call to the selected backend.
3. Keep `probeSystemIncludeDirs` as `LibClangSystemIncludes` implementation.
4. Add `FallbackSystemIncludes` for existing `/usr/include` and `/usr/local/include` behavior, used only when explicit policy says host headers are acceptable.
5. Update `docs/18-c-interop.md` section "System C headers" to describe backend-dependent discovery and the future WASM bundle.

Commands:

    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build -R cinterop --output-on-failure

Expected output:

- `test-cinterop` passes all existing system include tests.

Failure checks:

- If `test-cinterop` expects `stdint.h` present and this build chooses a non-libclang backend without a sysroot, the test must distinguish "no libclang" from "no discovered system include"; update the test only after reproducing the exact output.
- If `zithc info` reports different system include dirs than before, verify those dirs exist and capture the old/new output in the commit message.

Success criteria:

- `systemIncludeDirs` returns the same practical result as today on Linux with libclang.
- The libclang code is no longer the only implementation visible to the public function.

## Step 6 - Define the CHeaderParser interface and libclang adapter

Goal: the existing libclang implementation becomes one adapter behind a stable C header parser interface.

Sub-steps:

1. Create `/home/diogo/Zith/src/cinterop/c-header-parser.hpp`.
2. Define `struct CHeaderParserOptions` reusing the existing `zith::cinterop::ParseOptions`.
3. Define `class CHeaderParser { virtual std::shared_ptr<const CHeaderArtifact> parse(headerPath, options) = 0; virtual std::string name() const = 0; };`.
4. In `/home/diogo/Zith/src/cinterop/c-header.cpp`, keep `parseHeader` as the public entry point.
5. Rename the existing libclang implementation inside `c-header.cpp` to `LibClangHeaderParser` and make `parseHeader` dispatch through `ToolchainRegistry`.
6. When no parser is available, keep the existing diagnostic message about `extern fn`.
7. Do not disable `ZITH_ENABLE_C_INTEROP` at configure time in this step.

Commands:

    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build -R 'cinterop|frontend-modern' --output-on-failure

Expected output:

- The same tests that passed before still pass.
- A user-visible `zithc info` entry names the active header parser.

Failure checks:

- If `CHeaderArtifact` is currently not copyable and the adapter requires shared ownership, keep it heap-allocated with `std::shared_ptr` exactly as today.
- If removing the inner static helper breaks diagnostics, preserve the helper under the adapter and do not reformat it.

Success criteria:

- `parseHeader` works identically with libclang enabled.
- A new backend can be added without changing `parseHeader` callers.

## Step 7 - Implement the Zith-owned C binder for the validated surface

Goal: `/home/diogo/Zith/src/cinterop/c-binder.cpp` parses a defined subset of C headers without libclang and produces the same `CHeaderArtifact`, using only the lexer/parser utilities already present in the repo.

Sub-steps:

1. Create `/home/diogo/Zith/src/cinterop/c-binder.hpp`.
2. Declare `class ZithCBinder final : public CHeaderParser`.
3. Implement a tokenizer that recognizes C identifiers, keywords, punctuation, numeric literals, string literals, char literals, comments, and preprocessor lines.
4. Implement a declaration parser that accepts at minimum: `void`, integer/float types, pointers, `const`, function prototypes, simple records, enums, and object-like scalar macros.
5. Reuse the existing literal rules from `support::parseIntegerLiteral` and the `Scalars` logic in `c-header.cpp`; do not duplicate suffix tables.
6. For every type or declaration outside the implemented subset, return a diagnostic and put the name in `artifact->skippedFunctions` with a reason.
7. Keep `ZITH_ENABLE_C_INTEROP=OFF` or WASM builds able to use `ZithCBinder` even without libclang.
8. Register `ZithCBinder` in `ToolchainRegistry` with `HeaderImport` capability.
9. Add `tests/test-c-binder.cpp` mirroring the scalar, pointer, macro, and variadic cases already covered by `tests/test-cinterop.cpp`.
10. Do not yet add struct-by-value ABI emission in this step; bindings may parse records into `TypeKind::Record` but calls that need value ABI must report unsupported until Step 8.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build -DZITH_ENABLE_C_INTEROP=OFF
    cmake --build /home/diogo/Zith/build -j4
    ./build/tests/test-c-binder
    ctest --test-dir /home/diogo/Zith/build -R 'c-binder|cinterop' --output-on-failure

Expected output:

- `test-c-binder` passes with `ZITH_ENABLE_C_INTEROP=OFF`.
- All accepted cases in the new binder have no diagnostics and the expected functions/constants.

Failure checks:

- If the new binder accepts a declaration that `test-cinterop` rejects today, compare the exact reason and adjust the binder to reject first.
- If a source line from `c-header.cpp` is duplicated and later diverges, extract the shared helper to `src/cinterop/c-common.hpp` instead of copying it.
- If CMake does not add the new `.cpp` because of the source glob, rerun `cmake -S /home/diogo/Zith -B /home/diogo/Zith/build` and confirm `src/cinterop/c-binder.cpp` appears in `build/compile_commands.json`.

Success criteria:

- `import "header.h"` for the validated surface works on a build with `ZITH_ENABLE_C_INTEROP=OFF`.
- Unsupported C constructs produce explicit diagnostics instead of silently producing wrong bindings.

## Step 8 - Validate simple struct-by-value ABI

Goal: the binder either proves ABI correctness for simple records passed by value or rejects them; it never imports unverified struct ABI.

Sub-steps:

1. Add to `CHeaderParserOptions` a target triple and sysroot that the binder can use for ABI decisions.
2. Implement record layout only for simple records whose fields are scalar types, plain pointers, or nested records already validated; reject bitfields, explicit packing, anonymous records, flexible arrays, and `long double`/`__int128` edge cases.
3. Add ABI metadata fields to the internal `Type` representation or a side table for simple records used by value.
4. Have the lowerer reject any call with a record value whose layout/alignment is not present in that metadata.
5. Add tests for `x86_64-linux-gnu` and `aarch64-linux-gnu` covering a small struct passed by value and returned by value.
6. Keep libclang behavior unchanged for hosts with libclang until the new binder reaches parity on the same tests.
7. Update `docs/18-c-interop.md` to list the exact supported record subset.

Commands:

    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build -R 'c-binder|codegen' --output-on-failure

Expected output:

- Negative tests fail compilation with a clear "unsupported C ABI" or "unverified ABI" diagnostic.
- Positive simple struct tests pass on the configured target.

Failure checks:

- If an ABI test passes only accidentally, inspect the generated LLVM IR for struct argument lowering and do not leave the test green without a layout assertion.
- If the record layout helper becomes target-specific, place it under `src/cinterop/abi/` with a `target` parameter, not as global hardcoded sizes.

Success criteria:

- The validated C ABI surface is documented and test-enforced.
- No struct-by-value case is accepted without a proven layout/alignment path.

## Step 9 - Add the WASM header bundle

Goal: WASM/`ZITH_IS_WASM` builds can resolve common C headers from a bundled `src/wasm/headers/` tree instead of host system headers.

Sub-steps:

1. Create `/home/diogo/Zith/src/wasm/headers/` with at least `stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`, `errno.h`, `limits.h`, `float.h`, and `stdalign.h`.
2. Add a CMake copy rule that installs that tree into the bundle location for WASM builds.
3. Add `WasmHeaderBundle` capability in `ToolchainRegistry`.
4. In `cinterop::systemIncludeDirs`, when `ZITH_IS_WASM` is set and `WasmHeaderBundle` is available, return the bundled directory as the first system include root.
5. Add an extension point in `ParseOptions::includeDirs` and `ZithProject.toml` so users can provide extra WASM headers later, but do not design the user-facing flow now.
6. Add `tests/test-c-binder.cpp` cases that parse `#include <stddef.h>` with `ZITH_IS_WASM`-style include paths on the host test build.
7. Update `docs/18-c-interop.md` and `docs/wasm-playground-abi.md` to state that WASM uses Zith's provided headers and does not read host `/usr/include`.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build-wasm -DZITH_IS_WASM=ON -DZITH_HAS_LLVM=OFF
    cmake --build /home/diogo/Zith/build-wasm -j4
    ctest --test-dir /home/diogo/Zith/build-wasm -R 'c-binder|cinterop' --output-on-failure

Expected output:

- WASM configure succeeds.
- The binder finds `stddef.h` in `/home/diogo/Zith/src/wasm/headers/`.
- The same tests pass on a host build configured with the header path.

Failure checks:

- If WASM build fails because codegen sources are removed, verify `ZITH_IS_WASM=ON` also removes `src/codegen/*.cpp` as documented in the root `CMakeLists.txt`.
- If `ZITH_HAS_LLVM=OFF` prevents `llvm::Triple` usage, guard all reference to `llvm::Triple` in the binder with `#ifdef ZITH_HAS_LLVM`.
- If the bundle duplicates host headers, keep only the minimal declarations the tests need; do not copy glibc headers into the repo.

Success criteria:

- WASM builds resolve common C headers without host include paths.
- A future user-supplied WASM header root can override the bundle through the planned interface.

## Step 10 - Add clear diagnostics and capabilities to `zithc info`

Goal: the CLI and compiler messages expose which toolchain pieces are present, which are bundled, and which operation is unavailable with a precise reason.

Sub-steps:

1. Open `/home/diogo/Zith/src/cli/cmd/info.cpp`.
2. Print a `Toolchain` section with rows for `Header parser`, `C compile backend`, `Linker backend`, `System includes`, `WASM headers`.
3. Define a helper in `src/cc/toolchain.cpp` called `describeAvailability()` returning one string per capability.
4. Replace runtime messages that say `use 'extern fn' instead` with a dynamic message naming the missing backend and the fallback, if any.
5. In `compileCSource()` and `linkNative()`, return a `CCompileDisabled`/`LinkFailed` diagnostic when the required capability is absent instead of silently calling a host process.
6. Keep the old external-driver path only behind an explicit `ToolchainBackend::ExternalDriver` registration or CLI flag.

Commands:

    cmake --build /home/diogo/Zith/build -j4
    ./build/zithc info
    ctest --test-dir /home/diogo/Zith/build --output-on-failure

Expected output:

- `zithc info` lists each toolchain capability and its backend.
- Full CTest suite passes the same count as before this step.

Failure checks:

- If an existing test expects `/usr/bin/cc` in output, update only that assertion when the fallback policy changed.
- If a diagnostic string is asserted in multiple tests, grep with `rg "use 'extern fn' instead" /home/diogo/Zith` and update all occurrences in the same commit.

Success criteria:

- Missing capabilities fail fast with actionable diagnostics.
- `zithc info` is the single place a user can inspect toolchain state.

## Step 11 - Retire the default TinyCC dependency

Goal: default native builds no longer fetch or prefer TinyCC, while experiments can still opt into `libtcc` explicitly.

Sub-steps:

1. In `CMakeLists.txt`, change `ZITH_TCC_FETCH` default from `ON` to `OFF`.
2. Keep `ZITH_TCC_ROOT` and `libtcc` detection for experimental builds.
3. Add CMake option `ZITH_EXPERIMENTAL_TINYCC` defaulting to `OFF`; when ON, keep the existing fetch/ExternalProject path.
4. In `ToolchainRegistry`, register `LibTcc` only when `ZITH_EXPERIMENTAL_TINYCC=ON` and the library is found.
5. Update `docs/18-c-interop.md`, `memory/build-c-compile.md`, and `README.md` to describe TinyCC as optional/experimental.
6. Keep `compileWithTcc` code under the experiment gate so it can be compared later with the tiny C backend.

Commands:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build -DZITH_EXPERIMENTAL_TINYCC=OFF
    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build --output-on-failure

Expected output:

- Configure no longer logs `fetching TinyCC 0.9.27` unless the experiment flag is ON.
- Full test suite passes or regresses only from a documented libtcc-dependent test.

Failure checks:

- If an existing test is gated on `ZITH_C_COMPILE_AVAILABLE` and breaks because TinyCC is off, update the test to construct its own experimental libtcc build instead of expecting it by default.
- If `ZITH_TCC_FETCH` is still referenced elsewhere, grep and update `AGENTS.md` and README copy in the same commit.

Success criteria:

- Default standalone builds have no TinyCC fetch/requirement.
- An explicit experiment can still build and compare against libtcc.

## Step 12 - Define and schedule the tiny C backend

Goal: the roadmap has explicit criteria and target scope for the future Zith-owned C backend, so it is not designed by accident during binder work.

Sub-steps:

1. Create `/home/diogo/Zith/docs/plans/tiny-c-backend.md`.
2. State the goal: emit correct simple C17 object files for native companions with small binary size and stable behavior; optimization is explicitly out of scope.
3. Define the first supported target as ELF x86-64 and aarch64, using the object-emission infrastructure expected after Step 8.
4. Define the first source surface: functions with scalar/pointer/simple-record ABI, `extern`, `static`, control flow, calls, arithmetic, and the same struct ABI rules as the binder.
5. Define the rejection surface: variable-length arrays, bitfields, `goto`, complex macros, `setjmp`/`sigsetjmp`, flexible arrays, and packed structs until separately validated.
6. Add success criteria: deterministic object files, embedded no-host build, CTest ABI tests, and no LLVM/libclang dependency in the backend runtime.
7. Do not implement backend code in this session; leave it as a future execution step.

Commands:

    ls /home/diogo/Zith/docs/plans/tiny-c-backend.md
    rg -n "tiny C backend|Tiny C backend" /home/diogo/Zith/docs/plans/tiny-c-backend.md

Expected output:

- The file exists and contains the exact target/surface/rejection criteria above.

Failure checks:

- If the plan adds optimization goals, remove them; the user explicitly excluded optimization focus.
- If the plan references TinyCC as a runtime dependency, replace it with an experimental comparison only.

Success criteria:

- A future executor can take `tiny-c-backend.md` and build the backend without reopening the ADRs.

## Final Acceptance

Run:

    cmake -S /home/diogo/Zith -B /home/diogo/Zith/build
    cmake --build /home/diogo/Zith/build -j4
    ctest --test-dir /home/diogo/Zith/build --output-on-failure
    ./build/zithc info

Accept only if:

- `zithc info` reports the active header parser, C compile backend, linker backend, system include provider, and WASM header bundle.
- No default native build reads TinyCC or requires a host C compiler unless explicitly enabled.
- The C header binder is available on non-libclang/WASM builds for the validated surface.
- Unvalidated C ABI constructs are rejected with actionable diagnostics.
- The ADRs in `/home/diogo/Zith/docs/adr/` match the implemented behavior.

Forbidden actions in every phase:

- Never run `git reset --hard` or `git checkout --`.
- Do not delete user changes in unrelated files.
- Do not make this plan depend on a sysroot/libc bundle; the OS libc remains the deployment assumption for now.
- Do not change Zith language semantics while implementing toolchain changes.
