#!/usr/bin/env python3
"""Gzip-compress a file. Used at CMake configure time to embed the web assets."""

import gzip
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input> <output>")
        sys.exit(1)

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])

    data = src.read_bytes()
    with gzip.open(dst, "wb", compresslevel=9) as f:
        f.write(data)

    print(f"gzip: {src.name} {len(data)} -> {dst.stat().st_size} bytes")


if __name__ == "__main__":
    main()
