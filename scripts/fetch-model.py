#!/usr/bin/env python3
"""Explicit setup only: fetch the pinned ~178 MB Redux weights; never used by runtime."""
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap.model_files import FILES, REVISION


def matches(path, size, digest):
    if not path.is_file() or path.is_symlink() or path.stat().st_size != size:
        return False
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() == digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    dest = args.destination.expanduser().absolute()
    if dest.is_symlink():
        parser.error("destination must not be a symlink")
    dest.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name, (size, digest) in FILES.items():
        target = dest / name
        if matches(target, size, digest):
            continue
        if target.exists() or target.is_symlink():
            parser.error(f"existing mismatched file: {target}; move it aside explicitly")
        url = f"https://huggingface.co/moondream/parakeet-redux/resolve/{REVISION}/{name}"
        fd, temp = tempfile.mkstemp(prefix=".download-", dir=dest)
        try:
            with os.fdopen(fd, "wb") as output, urllib.request.urlopen(url, timeout=60) as source:
                total = 0
                while chunk := source.read(1024 * 1024):
                    total += len(chunk)
                    if total > size:
                        raise ValueError("download exceeded pinned size")
                    output.write(chunk)
            if not matches(Path(temp), size, digest):
                raise ValueError(f"pinned SHA-256/size mismatch: {name}")
            os.replace(temp, target)
        finally:
            Path(temp).unlink(missing_ok=True)
    print(f"Pinned Redux {REVISION} verified in {dest}")
    print("Model attribution: moondream/parakeet-redux, CC-BY-4.0; see downloaded README.md.")


if __name__ == "__main__":
    main()
