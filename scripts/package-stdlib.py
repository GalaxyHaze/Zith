#!/usr/bin/env python3
"""Create the deterministic stdlib pack consumed by the WASM playground."""

from pathlib import Path
import struct
import sys


MAGIC = b"ZSTDLIB2"
ABI_VERSION = 3


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <stdlib-dir> <output>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    output = Path(sys.argv[2])
    files = sorted(path for path in root.rglob("*.zithm") if path.is_file())
    payload = bytearray(MAGIC)
    payload += struct.pack("<II", ABI_VERSION, len(files))
    for path in files:
        relative = path.relative_to(root).as_posix().encode("utf-8")
        text = path.read_bytes()
        payload += struct.pack("<II", len(relative), len(text))
        payload += relative
        payload += text

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
