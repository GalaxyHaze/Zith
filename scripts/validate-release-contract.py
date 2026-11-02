#!/usr/bin/env python3
"""Validate release metadata consumed by installers and package managers."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import urlparse


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
ARCHITECTURES = {
    "64bit": "zithc-windows-amd64.exe",
    "arm64": "zithc-windows-arm64.exe",
}
RELEASE_ASSETS = (
    "zithc-linux-amd64",
    "zithc-linux-arm64",
    "zithc-linux-amd64-musl",
    "zithc-linux-arm64-musl",
    "zithc-macos-amd64",
    "zithc-macos-arm64",
    "zith-macos-runtime-amd64.tar.gz",
    "zith-macos-runtime-arm64.tar.gz",
    "zithc-windows-amd64.exe",
    "zithc-windows-arm64.exe",
    "zithc-stdlib-${TAG}.tar.gz",
    "zithc-stdlib-${TAG}.zip",
    "zithc-wasm.zip",
    "zith-lsp-linux-amd64",
    "zith-lsp-linux-arm64",
    "zith-lsp-macos-amd64",
    "zith-lsp-macos-arm64",
    "zith-lsp-windows-amd64.exe",
    "zith-lsp-windows-arm64.exe",
)


def fail(message: str) -> None:
    raise ValueError(message)


def validate_url(url: object, version: str, expected_asset: str) -> None:
    if not isinstance(url, str):
        fail(f"URL for {expected_asset} is not a string")
    parsed = urlparse(url)
    if parsed.scheme != "https" or parsed.netloc != "github.com":
        fail(f"URL for {expected_asset} is not a GitHub HTTPS URL: {url}")
    expected_suffix = f"/releases/download/v{version}/{expected_asset}"
    if not parsed.path.endswith(expected_suffix):
        fail(f"URL for {expected_asset} does not target v{version}: {url}")


def validate_hash(value: object, label: str, allow_empty: bool) -> None:
    if value == "" and allow_empty:
        return
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        fail(f"{label} must be a lowercase SHA-256 hash")


def validate_manifest(path: Path, allow_empty_hashes: bool) -> None:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read JSON manifest {path}: {error}")

    version = manifest.get("version")
    if not isinstance(version, str) or not SEMVER.fullmatch(version):
        fail("manifest version must be semantic version text without a leading v")

    architectures = manifest.get("architecture")
    if not isinstance(architectures, dict):
        fail("manifest architecture must be an object")

    for architecture, binary_name in ARCHITECTURES.items():
        entry = architectures.get(architecture)
        if not isinstance(entry, dict):
            fail(f"missing architecture entry: {architecture}")
        urls = entry.get("url")
        hashes = entry.get("hash")
        if not isinstance(urls, list) or len(urls) != 2:
            fail(f"{architecture}.url must contain the binary and stdlib URLs")
        if not isinstance(hashes, list) or len(hashes) != 2:
            fail(f"{architecture}.hash must contain binary and stdlib hashes")
        validate_url(urls[0], version, binary_name)
        validate_url(urls[1], version, f"zithc-stdlib-v{version}.zip")
        validate_hash(hashes[0], f"{architecture}.hash[0]", allow_empty_hashes)
        validate_hash(hashes[1], f"{architecture}.hash[1]", allow_empty_hashes)

    autoupdate = manifest.get("autoupdate", {}).get("architecture", {})
    for architecture, binary_name in ARCHITECTURES.items():
        urls = autoupdate.get(architecture, {}).get("url")
        if not isinstance(urls, list) or len(urls) != 2:
            fail(f"autoupdate entry is incomplete for {architecture}")
        if f"zithc-stdlib-v$version.zip" not in urls[1]:
            fail(f"autoupdate stdlib URL is invalid for {architecture}")
        if binary_name not in urls[0] or "v$version" not in urls[0]:
            fail(f"autoupdate binary URL is invalid for {architecture}")

    installer = manifest.get("installer", {}).get("script", [])
    installer_text = "\n".join(installer) if isinstance(installer, list) else ""
    for marker in (
        "$StdlibSource = Join-Path $dir 'std'",
        "Test-Path -LiteralPath $StdlibSource -PathType Container",
        "Copy-Item -LiteralPath $StdlibSource -Destination $StdlibDir -Recurse -Force",
        "Get-ChildItem $StdlibDir -Force | Remove-Item -Recurse -Force",
    ):
        if marker not in installer_text:
            fail(f"installer must use Scoop-extracted stdlib content: {marker}")
    if "Expand-Archive" in installer_text:
        fail("Scoop installer must not re-extract the stdlib archive")
    if "Remove-Item" not in installer_text:
        fail("installer must remove stale stdlib files")


def validate_workflows(workflow_dir: Path) -> None:
    release_page = workflow_dir / "create-new-release.yml"
    artifact_workflow = workflow_dir / "build-artifact.yml"
    maintenance_workflow = workflow_dir / "release-maintenance.yml"
    smoke_workflow = workflow_dir / "smoke-installers.yml"
    ci_workflow = workflow_dir / "ci.yml"
    repair_workflow = workflow_dir / "update-package.yml"
    wasm_installer = workflow_dir.parent.parent / "scripts/install-wasm.sh"
    try:
        release_text = release_page.read_text(encoding="utf-8")
        artifact_text = artifact_workflow.read_text(encoding="utf-8")
        maintenance_text = maintenance_workflow.read_text(encoding="utf-8")
        smoke_text = smoke_workflow.read_text(encoding="utf-8")
        ci_text = ci_workflow.read_text(encoding="utf-8")
        repair_text = repair_workflow.read_text(encoding="utf-8")
        wasm_installer_text = wasm_installer.read_text(encoding="utf-8")
        formula_text = (workflow_dir.parent / "homebrew/zithc.rb").read_text(
            encoding="utf-8"
        )
    except OSError as error:
        fail(f"cannot read release workflow contract: {error}")

    if "/master/" in release_text:
        fail(f"{release_page} contains an obsolete master-branch installer URL")
    if "git/refs" in maintenance_text or 'refs/tags/$RELEASE_TAG' in maintenance_text:
        fail(f"{maintenance_workflow} must not create a tag and retrigger artifact builds")
    if 'CUSTOM_RELEASE_BASE_URL="${ZITH_RELEASE_BASE_URL:-}"' not in wasm_installer_text:
        fail(f"{wasm_installer} does not preserve whether the release URL was customized")
    if '-z "$CUSTOM_RELEASE_BASE_URL"' not in wasm_installer_text:
        fail(f"{wasm_installer} cannot select authenticated draft asset downloads")
    expected_installers = (
        "/main/scripts/install.sh",
        "/main/scripts/install.ps1",
        "/main/scripts/install-wasm.sh",
    )
    for installer_path in expected_installers:
        if installer_path not in release_text:
            fail(f"{release_page} is missing the {installer_path} URL")
    if release_text.count("/main/") != 4:
        fail(f"{release_page} must publish four installer URLs from main")
    if "version: ${{ steps.tag.outputs.version }}" not in release_text:
        fail(f"{release_page} does not expose the normalized release version")
    if "tag_name: v${{ needs.tag-and-release.outputs.version }}" not in release_text:
        fail(f"{release_page} passes the raw version input to build-artifact")
    if "draft: true" not in release_text:
        fail(f"{release_page} exposes the release before verification gates pass")
    if "draft: false" not in release_text.split("secrets: inherit", 1)[0]:
        fail(f"{release_page} does not request final publication after verification")
    for installer in (
        Path("scripts/install.sh"),
        Path("scripts/install.ps1"),
        Path("scripts/install-wasm.sh"),
    ):
        installer_path = workflow_dir.parent.parent / installer
        try:
            installer_text = installer_path.read_text(encoding="utf-8")
        except OSError as error:
            fail(f"cannot read installer {installer_path}: {error}")
        if "GalaxyHaze/Zith-Lang" not in installer_text:
            fail(f"{installer_path} does not use the canonical repository")
    powershell_installer = (workflow_dir.parent.parent / "scripts/install.ps1").read_text(
        encoding="utf-8"
    )
    if (
        "Failed to download required asset '$FailedAsset' for release '$Version' from $FailedUrl"
        not in powershell_installer
        or "$($DownloadError.Exception.Message)" not in powershell_installer
    ):
        fail("scripts/install.ps1 hides the release asset download error")
    shell_installer = (workflow_dir.parent.parent / "scripts/install.sh").read_text(
        encoding="utf-8"
    )
    if "--lsp" not in shell_installer or "zith-lsp-" not in shell_installer:
        fail("scripts/install.sh does not support optional LSP installation")
    if "[switch]$Lsp" not in powershell_installer or "zith-lsp-" not in powershell_installer:
        fail("scripts/install.ps1 does not support optional LSP installation")
    if "create_draft=true" not in artifact_text:
        fail(f"{artifact_workflow} does not force newly created releases to start as drafts")
    if "matrix.msvc_arch == ''" in artifact_text:
        fail(f"{artifact_workflow} contains an unreachable empty-architecture branch")
    if "msvc_arch: arm64" not in artifact_text:
        fail(f"{artifact_workflow} does not declare the native Windows ARM64 target")
    if artifact_text.count('-DLLVM_DIR="C:/Program Files/LLVM/lib/cmake/llvm"') < 4:
        fail(f"{artifact_workflow} does not provide LLVM_DIR for all Windows targets")
    if "-DZITH_HAS_LLVM=OFF" in artifact_text:
        fail(f"{artifact_workflow} disables LLVM for a release artifact")
    if artifact_text.count("Install Windows LLVM development archive") != 2:
        fail(f"{artifact_workflow} does not install Windows LLVM development archives")
    if "clang+llvm-$llvmVersion-$llvmArch-pc-windows-msvc.tar.xz" not in artifact_text:
        fail(f"{artifact_workflow} does not select complete Windows LLVM archives")
    if artifact_text.count("$llvmVersion = '20.1.8'") < 2:
        fail(f"{artifact_workflow} does not pin the Windows LLVM archive version")
    if artifact_text.count("LLVM archive did not contain LLVMConfig.cmake") < 2:
        fail(f"{artifact_workflow} does not verify the Windows LLVM CMake package")
    if artifact_text.count("name: Legacy LLVM installer (disabled)\n        if: false") != 2:
        fail(f"{artifact_workflow} does not disable the incomplete LLVM 18 installers")
    if artifact_text.count("name: Install Windows ARM64 dependencies\n        if: false") != 2:
        fail(f"{artifact_workflow} does not disable the incomplete LLVM 18 ARM64 installers")

    def job_section(text: str, job: str, workflow: Path = artifact_workflow) -> str:
        start = text.find(f"\n  {job}:")
        if start < 0:
            fail(f"{workflow} is missing the {job} job")
        next_match = re.search(r"\n  [A-Za-z0-9_-]+:\n", text[start + 1 :])
        end = start + 1 + next_match.start() if next_match else len(text)
        return text[start:end]

    arm64_llvm_section = job_section(artifact_text, "build-windows-arm64-llvm")
    if "LLVM_TARGETS_TO_BUILD=AArch64;X86;WebAssembly" not in arm64_llvm_section:
        fail(f"{artifact_workflow} does not build all LLVM codegen targets for Windows ARM64")
    for marker in (
        "LLVMX86CodeGen.lib",
        "LLVMAArch64CodeGen.lib",
        "LLVMWebAssemblyCodeGen.lib",
        "actions/cache@v4",
        "actions/upload-artifact@v4",
    ):
        if marker not in arm64_llvm_section:
            fail(f"{artifact_workflow} Windows ARM64 LLVM job is missing {marker}")
    if (
        "git clone --depth 1 --branch $llvmTag "
        "https://github.com/llvm/llvm-project.git llvm-project"
        not in arm64_llvm_section
    ):
        fail(f"{artifact_workflow} does not fetch pinned LLVM sources on an ARM64 cache miss")
    for marker in (
        "$cmakeCompilerPath = $clangCl -replace '\\\\', '/'",
        "$cmakeInstallPath = $installRoot -replace '\\\\', '/'",
        '"-DCMAKE_C_COMPILER=$cmakeCompilerPath"',
        '"-DCMAKE_CXX_COMPILER=$cmakeCompilerPath"',
        '"-DCMAKE_INSTALL_PREFIX=$cmakeInstallPath"',
    ):
        if marker not in arm64_llvm_section:
            fail(f"{artifact_workflow} does not normalize Windows paths passed to CMake: {marker}")

    for job in ("build-main", "build-musl", "build-lsp"):
        section = job_section(artifact_text, job)
        if "-DZITH_HAS_LLVM=ON" not in section:
            fail(f"{artifact_workflow} does not enable LLVM in {job}")
        if "-DZITH_REQUIRE_LLVM=ON" not in section:
            fail(f"{artifact_workflow} does not require LLVM in {job}")
        if "verify-llvm-build.py" not in section:
            fail(f"{artifact_workflow} does not verify LLVM in {job}")
    main_section = job_section(artifact_text, "build-main")
    lsp_section = job_section(artifact_text, "build-lsp")
    if (
        "actions/upload-artifact@v4" not in main_section
        or "compiler-${{ matrix.target_name }}" not in main_section
        or "if: runner.os == 'macOS'" not in main_section
    ):
        fail(f"{artifact_workflow} does not stage macOS compiler binaries internally")
    if "actions/download-artifact@v4" not in lsp_section:
        fail(f"{artifact_workflow} does not retrieve the macOS compiler for runtime packaging")
    if "scripts/package-macos-runtime.py" not in lsp_section:
        fail(f"{artifact_workflow} does not package macOS transitive dependencies")
    if "needs: [create-release, build-windows-arm64-llvm, build-main]" not in lsp_section:
        fail(f"{artifact_workflow} packages macOS runtimes before compiler builds finish")
    for marker in ("-headerpad_max_install_names", "zith-macos-runtime-"):
        if marker not in artifact_text:
            fail(f"{artifact_workflow} is missing macOS runtime marker {marker}")
    for job in ("build-main", "build-musl", "package-stdlib", "build-lsp", "build-wasm"):
        section = job_section(artifact_text, job)
        if "uses: softprops/action-gh-release@v2" in section and "draft: true" not in section:
            fail(f"{artifact_workflow} may publish assets before gates pass in {job}")
    musl_section = job_section(artifact_text, "build-musl")
    if (
        "needs: [create-release, build-windows-arm64-llvm, build-main]" not in musl_section
        or "target_name: zithc-linux-amd64" not in job_section(artifact_text, "build-main")
    ):
        fail(f"{artifact_workflow} does not gate musl on LLVM and the native Linux x64 build")
    if "alpine:3.22" not in musl_section:
        fail(f"{artifact_workflow} does not build musl artifacts in Alpine")
    if "apk add --no-cache" not in musl_section:
        fail(f"{artifact_workflow} does not install Alpine build dependencies")
    for package in (
        "clang20",
        "llvm20-dev",
        "llvm20-static",
        "llvm20-gtest",
        "lld20",
        "git",
        "zlib-static",
    ):
        if package not in musl_section:
            fail(f"{artifact_workflow} does not install {package} for musl builds")
    if "-DZLIB_USE_STATIC_LIBS=ON" not in musl_section:
        fail(f"{artifact_workflow} does not select static zlib for musl builds")
    zlib_markers = (
        'zlib_archive="$(find /usr/lib -name libz.a -print -quit)"',
        'test -n "$zlib_archive" && test -f "$zlib_archive"',
        '-DZLIB_LIBRARY="$zlib_archive"',
        "zlib_cache=\"$(sed -n 's/^ZLIB_LIBRARY:[^=]*=//p' build/CMakeCache.txt)\"",
        'if [ "$zlib_cache" != "$zlib_archive" ]; then',
        "case \"$zlib_cache\" in",
        "*.a) ;;",
    )
    for marker in zlib_markers:
        if marker not in musl_section:
            fail(f"{artifact_workflow} does not verify static zlib selection: {marker}")
    archive_discovery = musl_section.find(zlib_markers[0])
    cmake_configure = musl_section.find("cmake -S . -B build")
    zlib_library_arg = musl_section.find(zlib_markers[2])
    zlib_cache_read = musl_section.find(zlib_markers[3])
    zlib_cache_match = musl_section.find(zlib_markers[4])
    zlib_archive_check = musl_section.find(zlib_markers[5])
    cmake_build = musl_section.find("cmake --build build --config Release")
    if not (
        0 <= archive_discovery
        < cmake_configure
        < zlib_library_arg
        < zlib_cache_read
        < zlib_cache_match
        < zlib_archive_check
        < cmake_build
    ):
        fail(f"{artifact_workflow} does not check the zlib cache before building")
    for platform in ("linux/amd64", "linux/arm64"):
        if f"container_platform: {platform}" not in musl_section:
            fail(f"{artifact_workflow} does not build musl for {platform}")
    if "clang-20 --print-target-triple" not in musl_section:
        fail(f"{artifact_workflow} does not detect the Alpine compiler target")
    if '--expected-target "$compiler_target"' not in musl_section:
        fail(f"{artifact_workflow} does not verify the Alpine compiler target")
    if "zig_target" in musl_section or "ZIG_VERSION" in artifact_text:
        fail(f"{artifact_workflow} retains the removed Zig musl toolchain")
    if "needs: [create-release, build-windows-arm64-llvm]" not in job_section(
        artifact_text, "build-main"
    ):
        fail(f"{artifact_workflow} does not gate build-main on full-backend LLVM")
    for job in ("build-main", "build-lsp"):
        section = job_section(artifact_text, job)
        if "llvm_asset: win64" not in section or "llvm_asset: woa64" not in section:
            fail(f"{artifact_workflow} does not select Windows LLVM assets per architecture in {job}")
        if "Install Windows build tools" not in section:
            fail(f"{artifact_workflow} does not install Windows build tools in {job}")
        if "Install Windows LLVM development archive" not in section:
            fail(f"{artifact_workflow} does not install a Windows LLVM development archive in {job}")
        if "tar.exe -xf $archive" not in section:
            fail(f"{artifact_workflow} does not extract the Windows LLVM archive in {job}")
        if "Remove-Item -LiteralPath $llvmRoot" not in section:
            fail(f"{artifact_workflow} does not clear stale LLVM files in {job}")
        if "Expected Clang 20" not in section:
            fail(f"{artifact_workflow} does not verify the Windows Clang version in {job}")
        if "Select-Object -First 1" not in section:
            fail(f"{artifact_workflow} does not isolate Clang version output in {job}")
        if "Download full-backend LLVM package" not in section:
            fail(f"{artifact_workflow} does not download full-backend LLVM in {job}")
        if "LLVMWebAssemblyCodeGen.lib" not in section:
            fail(f"{artifact_workflow} does not verify the Windows ARM64 WebAssembly backend in {job}")
        if "Expose DIA SDK at LLVM's configured path" not in section:
            fail(f"{artifact_workflow} does not expose the DIA SDK to LLVM in {job}")
        if (
            "VSINSTALLDIR" not in section
            or "diaguids.lib" not in section
            or "Select-String -Pattern $pattern" not in section
            or "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise" in section
        ):
            fail(f"{artifact_workflow} does not resolve the LLVM DIA SDK path dynamically in {job}")
    publish_section = job_section(artifact_text, "publish-release")
    if "if: needs.create-release.outputs.draft == 'false'" not in publish_section:
        fail(f"{artifact_workflow} does not gate final publication on draft mode")
    if "--draft=false" not in publish_section:
        fail(f"{artifact_workflow} does not publish only after the release gates")
    wasm_section = job_section(artifact_text, "build-wasm")
    if "-DZITH_IS_WASM=ON" not in wasm_section:
        fail(f"{artifact_workflow} does not configure the WASM release job")
    if "-DZITH_REQUIRE_LLVM=ON" in wasm_section:
        fail(f"{artifact_workflow} requires native LLVM in the WASM job")

    validation_section = job_section(artifact_text, "validate-release-assets")
    if "actions/checkout@v4" not in validation_section:
        fail(f"{artifact_workflow} does not checkout the release asset validator")
    if "scripts/validate-release-assets.py" not in validation_section:
        fail(f"{artifact_workflow} does not use the release asset validator")
    for asset in RELEASE_ASSETS:
        if asset not in validation_section:
            fail(f"{artifact_workflow} does not validate release asset {asset}")
    maintenance_job = job_section(artifact_text, "release-maintenance")
    for marker in (
        "needs: [create-release, validate-release-assets]",
        "uses: ./.github/workflows/release-maintenance.yml",
        "release_tag: ${{ needs.create-release.outputs.tag }}",
        "source_ref: ${{ github.sha }}",
        "run_install_smokes: true",
        "secrets: inherit",
    ):
        if marker not in maintenance_job:
            fail(f"{artifact_workflow} release maintenance call is missing {marker}")

    distribution_section = job_section(
        maintenance_text, "update-distribution", maintenance_workflow
    )
    if "needs: [validate-release-source]" not in distribution_section:
        fail(f"{maintenance_workflow} distribution job is not gated on release validation")
    if "group: zith-distribution-metadata" not in distribution_section:
        fail(f"{maintenance_workflow} does not serialize distribution metadata updates")
    for marker in (
        "scripts/update-scoop-manifest.py",
        "scripts/update-homebrew-formula.py",
        "zithc-stdlib-$RELEASE_TAG.zip",
        'archive/${SOURCE_SHA}.tar.gz',
        "sha256sum source-archive.tar.gz",
        "--sha256 \"$SHA\"",
        '--source-ref "$SOURCE_SHA"',
        "Sync Homebrew tap formula",
        "base64 --wrap=0 .github/homebrew/zithc.rb",
        "contents/Formula/zithc.rb",
        "gh api --method PUT",
    ):
        if marker not in distribution_section:
            fail(f"{maintenance_workflow} is missing distribution step {marker}")

    maintenance_smoke = job_section(
        maintenance_text, "smoke-installers", maintenance_workflow
    )
    for marker in (
        "needs: [validate-release-source]",
        "if: inputs.run_install_smokes",
        "uses: ./.github/workflows/smoke-installers.yml",
        "release_tag: ${{ inputs.release_tag }}",
        "source_ref: ${{ github.sha }}",
        "secrets: inherit",
    ):
        if marker not in maintenance_smoke:
            fail(f"{maintenance_workflow} smoke call is missing {marker}")
    validate_source = job_section(
        maintenance_text, "validate-release-source", maintenance_workflow
    )
    for marker in (
        "ref: ${{ inputs.source_ref }}",
        'SOURCE_SHA=$(git rev-parse HEAD)',
        'gh release view "$RELEASE_TAG"',
    ):
        if marker not in validate_source:
            fail(f"{maintenance_workflow} does not validate the release source: {marker}")
    for job in ("update-distribution", "sync-playground-wasm"):
        section = job_section(maintenance_text, job, maintenance_workflow)
        if "needs: [validate-release-source]" not in section:
            fail(f"{maintenance_workflow} {job} is not gated on release validation")
    playground_section = job_section(
        maintenance_text, "sync-playground-wasm", maintenance_workflow
    )
    for marker in (
        'gh release download "$RELEASE_TAG"',
        "playground/zith-playground.wasm",
        "playground/zith-stdlib.pack",
        "playground/runtime.json",
        "git push origin gh-pages",
    ):
        if marker not in playground_section:
            fail(f"{maintenance_workflow} is missing playground sync step {marker}")

    if any(
        legacy_job in artifact_text
        for legacy_job in (
            "smoke-installers-unix:",
            "smoke-installer-windows:",
            "smoke-installer-wasm:",
        )
    ):
        fail(f"{artifact_workflow} still defines inline installer smoke jobs")

    if "smoke-scoop:" not in artifact_text:
        fail(f"{artifact_workflow} is missing the Scoop release smoke job")
    scoop_section = job_section(artifact_text, "smoke-scoop")
    if "needs: [create-release, release-maintenance]" not in scoop_section:
        fail(f"{artifact_workflow} runs Scoop smoke before release maintenance")
    dispatch_start = smoke_text.find("  workflow_dispatch:")
    call_start = smoke_text.find("  workflow_call:")
    if dispatch_start < 0 or call_start < 0 or call_start <= dispatch_start:
        fail(f"{smoke_workflow} must support manual and reusable invocation")
    dispatch_inputs = smoke_text[dispatch_start:call_start]
    call_inputs = smoke_text[call_start : smoke_text.find("\npermissions:", call_start)]

    def input_section(block: str, name: str) -> str:
        match = re.search(
            rf"(?m)^      {re.escape(name)}:\n((?:^        .*\n)*)",
            block,
        )
        if match is None:
            fail(f"{smoke_workflow} is missing the {name} input")
        return match.group(1)

    dispatch_release_tag = input_section(dispatch_inputs, "release_tag")
    dispatch_source_ref = input_section(dispatch_inputs, "source_ref")
    call_release_tag = input_section(call_inputs, "release_tag")
    call_source_ref = input_section(call_inputs, "source_ref")
    if "required: true" not in dispatch_release_tag:
        fail(f"{smoke_workflow} must require the release tag for manual runs")
    if "required: false" not in dispatch_source_ref or "default: main" not in dispatch_source_ref:
        fail(f"{smoke_workflow} must default the manual source ref to main")
    for name, section in (
        ("release_tag", call_release_tag),
        ("source_ref", call_source_ref),
    ):
        if "required: true" not in section:
            fail(f"{smoke_workflow} must require {name} from reusable callers")
    validation_job = job_section(smoke_text, "validate-inputs", smoke_workflow)
    if "v[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?" not in validation_job:
        fail(f"{smoke_workflow} does not validate release tags before using them")
    for smoke_job, runner in (
        ("smoke-installers-unix", "ubuntu-latest, macos-14"),
        ("smoke-installer-windows", "windows-latest"),
        ("smoke-installer-wasm", "ubuntu-latest"),
    ):
        section = job_section(smoke_text, smoke_job, smoke_workflow)
        if runner not in section:
            fail(f"{smoke_workflow} does not run {smoke_job} on {runner}")
        if "ref: ${{ inputs.source_ref || github.ref }}" not in section:
            fail(f"{smoke_workflow} does not checkout the requested source ref in {smoke_job}")
        if "persist-credentials: false" not in section:
            fail(f"{smoke_workflow} persists checkout credentials in {smoke_job}")
        if "ZITH_RELEASE_TAG: ${{ inputs.release_tag }}" not in section:
            fail(f"{smoke_workflow} does not pass the release tag safely in {smoke_job}")
        if "GITHUB_TOKEN: ${{ secrets.RELEASE_PAT }}" not in section:
            fail(f"{smoke_workflow} does not authenticate draft asset downloads in {smoke_job}")
    for smoke_job, output_path in (
        ("smoke-installers-unix", "${{ runner.temp }}/zith-install"),
        ("smoke-installer-windows", "${{ runner.temp }}\\zith-install"),
        ("smoke-installer-wasm", "${{ runner.temp }}/zith-wasm-install"),
    ):
        if output_path not in job_section(smoke_text, smoke_job, smoke_workflow):
            fail(f"{smoke_workflow} writes outside runner.temp in {smoke_job}")
    if "examples/loops-simple.zith" not in smoke_text:
        fail(f"{smoke_workflow} installer smoke uses no C-interop-free Zith program")
    if "test-import-console.zith" in smoke_text:
        fail(f"{smoke_workflow} installer smoke depends on optional libclang interop")
    if '"$ZITH_RELEASE_TAG" --lsp' not in smoke_text or "-Version \"$env:ZITH_RELEASE_TAG\" -Lsp" not in smoke_text:
        fail(f"{smoke_workflow} does not exercise optional LSP installation on Unix and Windows")
    if "runtime-manifest.json" not in smoke_text:
        fail(f"{smoke_workflow} does not verify the installed macOS runtime")
    publish_section = job_section(artifact_text, "publish-release")
    if "if: needs.create-release.outputs.draft == 'false'" not in publish_section:
        fail(f"{artifact_workflow} publishes without honoring draft mode")
    for prerequisite in ("release-maintenance", "smoke-scoop"):
        if prerequisite not in publish_section:
            fail(f"{artifact_workflow} publishes before {prerequisite} passes")

    published_smoke = job_section(artifact_text, "smoke-published-windows")
    for marker in (
        "needs: [create-release, publish-release]",
        "runs-on: windows-latest",
        "persist-credentials: false",
        "EXPECTED_RELEASE_TAG: ${{ needs.create-release.outputs.tag }}",
        "Remove-Item \"Env:$name\" -ErrorAction SilentlyContinue",
        "https://api.github.com/repos/GalaxyHaze/Zith-Lang/releases/latest",
        "if ($latest.tag_name -cne $expectedTag)",
        "https://github.com/GalaxyHaze/Zith-Lang/releases/download/$expectedTag/"
        "zithc-windows-amd64.exe",
        "(Get-Item $directDownload).Length -le 0",
        "examples/loops-simple.zith",
        "share/zith/stdlib/std/io/console.zith",
        "check $source",
        "$LASTEXITCODE -ne 0",
    ):
        if marker not in published_smoke:
            fail(f"{artifact_workflow} published Windows smoke is missing {marker}")
    if re.search(r"(?m)^\s*& \$installer -Version \$expectedTag\s*$", published_smoke) is None:
        fail(f"{artifact_workflow} does not exercise the versioned Windows installer")
    if re.search(r"(?m)^\s*& \$installer\s*$", published_smoke) is None:
        fail(f"{artifact_workflow} does not exercise the latest Windows installer")
    if "zith-versioned" not in published_smoke or "zith-latest" not in published_smoke:
        fail(f"{artifact_workflow} does not isolate versioned and latest Windows installs")
    for credential in ("GITHUB_TOKEN:", "GH_TOKEN:", "ZITH_RELEASE_BASE_URL:"):
        if credential in published_smoke:
            fail(f"{artifact_workflow} configures credentials for public download smoke: {credential}")

    if "build-musl" in ci_text and "-DZITH_HAS_LLVM=OFF" in ci_text:
        fail(f"{ci_workflow} disables LLVM in the musl build")
    if "-DZITH_REQUIRE_LLVM=ON" not in ci_text:
        fail(f"{ci_workflow} does not require LLVM for native builds")
    if "-DLLVM_DIR=#{llvm_dir}" not in formula_text:
        fail("Homebrew formula does not pass the keg-only LLVM CMake directory")
    if "-DZITH_REQUIRE_LLVM=ON" not in formula_text:
        fail("Homebrew formula does not require LLVM")
    for marker in (
        "scripts/update-scoop-manifest.py",
        "scripts/update-homebrew-formula.py",
        "curl --fail --silent --show-error --location --retry 3 --retry-all-errors",
        "sha256sum source-archive.tar.gz",
        "git add .github/scoop/bucket/zithc.json .github/homebrew/zithc.rb",
        "Sync Homebrew tap formula",
        "base64 --wrap=0 .github/homebrew/zithc.rb",
        "contents/Formula/zithc.rb",
        "gh api --method PUT",
    ):
        if marker not in repair_text:
            fail(f"{repair_workflow} is missing shared distribution step {marker}")
    if 'curl -sL "$URL" | sha256sum' in repair_text:
        fail(f"{repair_workflow} hashes an unchecked curl pipeline")
    if "group: zith-distribution-metadata" not in repair_text:
        fail(f"{repair_workflow} does not share the distribution concurrency group")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workflow-dir", type=Path)
    parser.add_argument("--allow-empty-hashes", action="store_true")
    args = parser.parse_args()
    try:
        validate_manifest(args.manifest, args.allow_empty_hashes)
        if args.workflow_dir is not None:
            validate_workflows(args.workflow_dir)
    except ValueError as error:
        print(f"release contract error: {error}", file=sys.stderr)
        return 1
    print(f"release contract valid: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
