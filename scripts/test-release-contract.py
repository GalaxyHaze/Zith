#!/usr/bin/env python3
"""Exercise release metadata generation without contacting GitHub."""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
MANIFEST = ROOT / ".github/scoop/bucket/zithc.json"
FORMULA = ROOT / ".github/homebrew/zithc.rb"
VERSION = "9.8.7"
REPOSITORY = "GalaxyHaze/Zith-Lang"


def run(*args: str, cwd: Path | None = None) -> None:
    subprocess.run(
        [sys.executable, *args],
        cwd=cwd or ROOT,
        check=True,
        env={**os.environ, "PYTHONPATH": str(SCRIPTS)},
    )


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_runtime_archive(path: Path, corrupt_hash: bool = False) -> None:
    library = b"test dylib payload"
    library_hash = "0" * 64 if corrupt_hash else hashlib.sha256(library).hexdigest()
    manifest = json.dumps(
        {"libraries": {"libLLVM.dylib": library_hash}}
    ).encode("utf-8")
    with tarfile.open(path, "w:gz") as archive:
        for name, content in (("lib/libLLVM.dylib", library), ("manifest.json", manifest)):
            member = tarfile.TarInfo(name)
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))


def write_stdlib_archives(directory: Path, version: str) -> None:
    console_source = b"// release installer fixture\n"
    tar_path = directory / f"zithc-stdlib-v{version}.tar.gz"
    with tarfile.open(tar_path, "w:gz") as archive:
        member = tarfile.TarInfo("std/io/console.zith")
        member.size = len(console_source)
        archive.addfile(member, io.BytesIO(console_source))

    zip_path = directory / f"zithc-stdlib-v{version}.zip"
    with zipfile.ZipFile(zip_path, "w") as archive:
        archive.writestr("std/io/console.zith", console_source)


def test_shell_installer(directory: Path, version: str) -> None:
    release_url = directory.as_uri()
    with tempfile.TemporaryDirectory(prefix="zith-install-smoke-") as raw_root:
        install_root = Path(raw_root) / "with-lsp"
        result = subprocess.run(
            ["bash", str(SCRIPTS / "install.sh"), f"v{version}", "--lsp"],
            cwd=ROOT,
            env={
                **os.environ,
                "ZITH_INSTALL_ROOT": str(install_root),
                "ZITH_RELEASE_BASE_URL": release_url,
            },
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"POSIX installer with --lsp failed:\n{result.stderr}")
        for relative in (
            "bin/zithc",
            "bin/zith-lsp",
            "share/zith/stdlib/std/io/console.zith",
        ):
            if not (install_root / relative).is_file():
                raise AssertionError(f"POSIX installer did not install {relative}")
        if not (install_root / "bin/zith-lsp").stat().st_mode & 0o111:
            raise AssertionError("POSIX installer did not mark zith-lsp executable")

    with tempfile.TemporaryDirectory(prefix="zith-install-smoke-") as raw_root:
        install_root = Path(raw_root) / "without-lsp"
        result = subprocess.run(
            ["bash", str(SCRIPTS / "install.sh"), f"v{version}"],
            cwd=ROOT,
            env={
                **os.environ,
                "ZITH_INSTALL_ROOT": str(install_root),
                "ZITH_RELEASE_BASE_URL": release_url,
            },
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"POSIX installer default mode failed:\n{result.stderr}")
        if not (install_root / "bin/zithc").is_file():
            raise AssertionError("POSIX installer did not install zithc by default")
        if (install_root / "bin/zith-lsp").exists():
            raise AssertionError("POSIX installer installed zith-lsp without --lsp")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zith-release-contract-") as raw_dir:
        directory = Path(raw_dir)
        release_assets = (
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
            "zith-lsp-linux-amd64",
            "zith-lsp-linux-arm64",
            "zith-lsp-macos-amd64",
            "zith-lsp-macos-arm64",
            "zith-lsp-windows-amd64.exe",
            "zith-lsp-windows-arm64.exe",
        )
        runtime_assets = {
            "zith-macos-runtime-amd64.tar.gz",
            "zith-macos-runtime-arm64.tar.gz",
        }
        for name in release_assets:
            if name not in runtime_assets:
                (directory / name).write_bytes(b"release asset\n")
        for name in runtime_assets:
            write_runtime_archive(directory / name)
        with zipfile.ZipFile(directory / "zithc-wasm.zip", "w") as archive:
            archive.writestr("zith-playground.wasm", b"wasm\n")
            archive.writestr("zith-stdlib.pack", b"stdlib\n")
        write_stdlib_archives(directory, VERSION)
        build_dir = directory / "build"
        build_dir.mkdir()
        (build_dir / "CMakeCache.txt").write_text(
            "\n".join(
                (
                    "ZITH_HAS_LLVM:BOOL=ON",
                    "ZITH_REQUIRE_LLVM:BOOL=ON",
                    "ZITH_DETECTED_LLVM_VERSION:INTERNAL=18.1.8",
                    "CMAKE_CXX_COMPILER_TARGET:STRING=x86_64-alpine-linux-musl",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        run(
            str(SCRIPTS / "verify-llvm-build.py"),
            "--build-dir",
            str(build_dir),
            "--expected-target",
            "x86_64-alpine-linux-musl",
        )
        mismatch = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "verify-llvm-build.py"),
                "--build-dir",
                str(build_dir),
                "--expected-target",
                "aarch64-alpine-linux-musl",
            ],
            cwd=ROOT,
            env={**os.environ, "PYTHONPATH": str(SCRIPTS)},
            check=False,
            capture_output=True,
            text=True,
        )
        if mismatch.returncode == 0 or "expected 'aarch64-alpine-linux-musl'" not in (
            mismatch.stdout + mismatch.stderr
        ):
            raise AssertionError("LLVM target verification accepted a mismatched target")
        run(
            str(SCRIPTS / "validate-release-assets.py"),
            "--release-dir",
            str(directory),
            "--tag",
            f"v{VERSION}",
        )
        test_shell_installer(directory, VERSION)
        missing_asset = directory / "zithc-windows-amd64.exe"
        missing_asset.unlink()
        missing_asset_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "validate-release-assets.py"),
                "--release-dir",
                str(directory),
                "--tag",
                f"v{VERSION}",
            ],
            cwd=ROOT,
            env={**os.environ, "PYTHONPATH": str(SCRIPTS)},
            check=False,
            capture_output=True,
            text=True,
        )
        missing_asset.write_bytes(b"amd64 test asset\n")
        if (
            missing_asset_result.returncode == 0
            or f"missing or empty release asset: {missing_asset}"
            not in missing_asset_result.stderr
        ):
            raise AssertionError("release asset validation accepted a missing Windows compiler")

        missing_runtime = directory / "zith-macos-runtime-amd64.tar.gz"
        missing_runtime.unlink()
        missing_runtime_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "validate-release-assets.py"),
                "--release-dir",
                str(directory),
                "--tag",
                f"v{VERSION}",
            ],
            cwd=ROOT,
            env={**os.environ, "PYTHONPATH": str(SCRIPTS)},
            check=False,
            capture_output=True,
            text=True,
        )
        write_runtime_archive(missing_runtime)
        if (
            missing_runtime_result.returncode == 0
            or f"missing or empty release asset: {missing_runtime}"
            not in missing_runtime_result.stderr
        ):
            raise AssertionError("release asset validation accepted a missing macOS runtime")

        corrupt_runtime = directory / "zith-macos-runtime-arm64.tar.gz"
        write_runtime_archive(corrupt_runtime, corrupt_hash=True)
        corrupt_runtime_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "validate-release-assets.py"),
                "--release-dir",
                str(directory),
                "--tag",
                f"v{VERSION}",
            ],
            cwd=ROOT,
            env={**os.environ, "PYTHONPATH": str(SCRIPTS)},
            check=False,
            capture_output=True,
            text=True,
        )
        write_runtime_archive(corrupt_runtime)
        if (
            corrupt_runtime_result.returncode == 0
            or "checksum mismatch for lib/libLLVM.dylib"
            not in corrupt_runtime_result.stderr
        ):
            raise AssertionError(
                "release asset validator did not report a corrupt macOS runtime: "
                f"exit={corrupt_runtime_result.returncode}, "
                f"stdout={corrupt_runtime_result.stdout!r}, "
                f"stderr={corrupt_runtime_result.stderr!r}"
            )

        for name, content in (
            ("zithc-windows-amd64.exe", b"amd64 test asset\n"),
            ("zithc-windows-arm64.exe", b"arm64 test asset\n"),
            ("zithc-stdlib-v" + VERSION + ".zip", b"stdlib test asset\n"),
        ):
            (directory / name).write_bytes(content)

        manifest = directory / "zithc.json"
        formula = directory / "zithc.rb"
        shutil.copyfile(MANIFEST, manifest)
        shutil.copyfile(FORMULA, formula)

        run(
            str(SCRIPTS / "update-scoop-manifest.py"),
            "--manifest",
            str(manifest),
            "--release-dir",
            str(directory),
            "--repository",
            REPOSITORY,
            "--version",
            VERSION,
        )
        run(
            str(SCRIPTS / "update-homebrew-formula.py"),
            "--formula",
            str(formula),
            "--repository",
            REPOSITORY,
            "--version",
            VERSION,
            "--sha256",
            "0123456789abcdef" * 4,
        )
        run(
            str(SCRIPTS / "validate-release-contract.py"),
            "--manifest",
            str(manifest),
            "--workflow-dir",
            str(ROOT / ".github/workflows"),
        )

        metadata = json.loads(manifest.read_text(encoding="utf-8"))
        stdlib_hash = digest(directory / f"zithc-stdlib-v{VERSION}.zip")
        for architecture, binary in (
            ("64bit", "zithc-windows-amd64.exe"),
            ("arm64", "zithc-windows-arm64.exe"),
        ):
            expected = [digest(directory / binary), stdlib_hash]
            if metadata["architecture"][architecture]["hash"] != expected:
                raise AssertionError(
                    f"unexpected hashes for {architecture}: "
                    f"{metadata['architecture'][architecture]['hash']}"
                )

        formula_text = formula.read_text(encoding="utf-8")
        expected_url = (
            f"https://github.com/{REPOSITORY}/archive/refs/tags/v{VERSION}.tar.gz"
        )
        for expected in (
            f'version "{VERSION}"',
            f'url "{expected_url}"',
            '-DZITH_VERSION=v#{version}',
            'assert_path_exists share/"zith/stdlib"',
            'shell_output("#{bin}/zithc --version")',
        ):
            if expected not in formula_text:
                raise AssertionError(f"missing Homebrew metadata: {expected}")
        if 'sha256 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"' not in formula_text:
            raise AssertionError("missing Homebrew SHA-256")

        source_formula = directory / "zithc-source-ref.rb"
        shutil.copyfile(FORMULA, source_formula)
        source_ref = "a" * 40
        run(
            str(SCRIPTS / "update-homebrew-formula.py"),
            "--formula",
            str(source_formula),
            "--repository",
            REPOSITORY,
            "--version",
            VERSION,
            "--sha256",
            "0123456789abcdef" * 4,
            "--source-ref",
            source_ref,
        )
        source_formula_text = source_formula.read_text(encoding="utf-8")
        source_ref_url = f"https://github.com/{REPOSITORY}/archive/{source_ref}.tar.gz"
        if f'url "{source_ref_url}"' not in source_formula_text:
            raise AssertionError("Homebrew formula did not use the supplied source commit")

    print("release contract integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
