#!/usr/bin/env python3
"""Validate the complete release asset set before distribution updates."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def expected_assets(version: str) -> tuple[str, ...]:
    return (
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
        f"zithc-stdlib-v{version}.tar.gz",
        f"zithc-stdlib-v{version}.zip",
        "zithc-wasm.zip",
        "zith-lsp-linux-amd64",
        "zith-lsp-linux-arm64",
        "zith-lsp-macos-amd64",
        "zith-lsp-macos-arm64",
        "zith-lsp-windows-amd64.exe",
        "zith-lsp-windows-arm64.exe",
    )


def fail(message: str) -> None:
    raise SystemExit(message)


def validate_wasm_archive(path: Path) -> None:
    try:
        with zipfile.ZipFile(path) as archive:
            members = set(archive.namelist())
            for required in ("zith-playground.mjs", "zith-playground.wasm", "zith-stdlib.pack"):
                if required not in members:
                    fail(f"{path} is missing {required}")
                if archive.getinfo(required).file_size == 0:
                    fail(f"{path} contains an empty {required}")
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot inspect WASM archive {path}: {error}")


def validate_macos_runtime_archive(path: Path) -> None:
    try:
        with tarfile.open(path, "r:gz") as archive:
            archive_members = archive.getmembers()
            members = {member.name: member for member in archive_members}
            if len(members) != len(archive_members):
                fail(f"{path} contains duplicate archive paths")
            for member in archive_members:
                if member.isdir() and member.name in ("lib", "lib/"):
                    continue
                if (
                    not member.isfile()
                    or member.name.startswith("/")
                    or ".." in Path(member.name).parts
                ):
                    fail(f"{path} contains an unsafe or unsupported entry: {member.name}")
            manifest_member = members.get("manifest.json")
            if manifest_member is None:
                fail(f"{path} is missing manifest.json")
            manifest_file = archive.extractfile(manifest_member)
            if manifest_file is None:
                fail(f"cannot read runtime manifest from {path}")
            manifest = json.load(manifest_file)
            if not isinstance(manifest, dict):
                fail(f"{path} has an invalid manifest.json")
            libraries = manifest.get("libraries")
            if not isinstance(libraries, dict) or not libraries:
                fail(f"{path} has no libraries in manifest.json")
            expected_members = {"manifest.json", "lib", "lib/"}
            for name, expected_hash in libraries.items():
                if (
                    not isinstance(name, str)
                    or Path(name).name != name
                    or not name.endswith(".dylib")
                    or not isinstance(expected_hash, str)
                    or not SHA256.fullmatch(expected_hash)
                ):
                    fail(f"{path} has an invalid library entry in manifest.json")
                expected_members.add(f"lib/{name}")
                member = members.get(f"lib/{name}")
                if member is None or member.size == 0:
                    fail(f"{path} is missing or has an empty lib/{name}")
                library_file = archive.extractfile(member)
                if library_file is None:
                    fail(f"cannot read lib/{name} from {path}")
                actual_hash = hashlib.sha256(library_file.read()).hexdigest()
                if actual_hash != expected_hash:
                    fail(f"{path} has a checksum mismatch for lib/{name}")
            if set(members) - expected_members:
                fail(f"{path} contains files not listed in manifest.json")
    except (OSError, tarfile.TarError, json.JSONDecodeError) as error:
        fail(f"cannot inspect macOS runtime archive {path}: {error}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()

    version = args.tag.removeprefix("v")
    if not SEMVER.fullmatch(version):
        fail(f"invalid release tag: {args.tag}")

    for name in expected_assets(version):
        path = args.release_dir / name
        if not path.is_file() or path.stat().st_size == 0:
            fail(f"missing or empty release asset: {path}")

    validate_wasm_archive(args.release_dir / "zithc-wasm.zip")
    for architecture in ("amd64", "arm64"):
        validate_macos_runtime_archive(
            args.release_dir / f"zith-macos-runtime-{architecture}.tar.gz"
        )
    print(f"release assets valid for v{version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
