#!/usr/bin/env python3
"""Explicit setup only: fetch the pinned ~178 MB Redux weights; never used by runtime."""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap.model_files import check_file, check_model, load_backends


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    dest = args.destination.expanduser().absolute()
    if dest.is_symlink():
        parser.error("destination must not be a symlink")
    backend = load_backends()["redux"]
    dest.mkdir(mode=0o700, parents=True, exist_ok=True)
    for item in backend.files:
        target = dest / item.path
        reason, _ = check_file(dest, item)
        if reason is None:
            continue
        if reason != "missing_files":
            parser.error(f"existing mismatched or unsafe file: {target}; move it aside explicitly")
        # This explicit Redux-only tool does not implement a generic model downloader.
        url = f"{backend.source}/resolve/{backend.revision}/{item.path}"
        fd, temp = tempfile.mkstemp(prefix=".download-", dir=dest)
        try:
            with os.fdopen(fd, "wb") as output, urllib.request.urlopen(url, timeout=60) as source:
                total = 0
                while chunk := source.read(1024 * 1024):
                    total += len(chunk)
                    if total > item.size:
                        raise ValueError("download exceeded pinned size")
                    output.write(chunk)
            if check_file(dest, item)[0] != "missing_files":
                parser.error(f"destination changed during download: {target}")
            # Check the temporary file using the same pinned verifier before install.
            if check_file(dest, type(item)(Path(temp).name, item.size, item.sha256))[0] is not None:
                raise ValueError(f"pinned SHA-256/size mismatch: {item.path}")
            os.replace(temp, target)
        finally:
            Path(temp).unlink(missing_ok=True)
    if check_model(backend, dest)["state"] != "installed_verified":
        raise ValueError("pinned model verification failed")
    print(f"Pinned Redux {backend.revision} verified in {dest}")
    print(f"Model attribution: {backend.attribution} License: {backend.license_id}.")


if __name__ == "__main__":
    main()
