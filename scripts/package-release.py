#!/usr/bin/env python3
"""Produce an offline release archive from an independently vetted ARM64 staging tree.

This tool does not build/download a runtime, model, native libraries, or licenses.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tarfile
import tempfile

ARCH = "linux-aarch64"
ALLOWED = {"bin", "lib", "assets", "python", "runtime", "model", "fonts", "licenses"}
REQUIRED = ("bin/frameyap", "runtime/bin/python3", "python/frameyap/worker.py",
            "assets/actions.json", "fonts/font.ttf", "licenses/THIRD_PARTY_NOTICES.txt")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stage", type=Path, required=True, help="vetted standalone payload directory")
    p.add_argument("--output", type=Path, required=True, help="existing output directory")
    p.add_argument("--version", required=True)
    p.add_argument("--arch", choices=[ARCH], required=True)
    p.add_argument("--model-revision", required=True, help="exact vetted model revision identifier")
    p.add_argument("--external-runtime", action="store_true", help="omit ASR runtime; user must supply an independently authorized Python environment")
    args = p.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", args.version) or args.version in (".", ".."):
        p.error("invalid version")
    if not args.model_revision.strip():
        p.error("model revision must be nonempty")
    stage = args.stage.resolve(strict=True)
    if not stage.is_dir() or args.stage.is_symlink():
        p.error("stage must be a real directory")
    if not args.output.is_dir() or args.output.is_symlink():
        p.error("output must be an existing real directory")
    if any(item.name not in ALLOWED for item in stage.iterdir()):
        p.error("stage has unknown root entries or preexisting release.json")
    for name in REQUIRED:
        if args.external_runtime and name == "runtime/bin/python3":
            continue
        if not (stage / name).is_file() or (stage / name).is_symlink():
            p.error(f"missing file: {name}")
    if args.external_runtime and (stage / "runtime").exists():
        p.error("external-runtime package must not contain a runtime directory")
    for name in (("bin/frameyap",) if args.external_runtime else ("bin/frameyap", "runtime/bin/python3")):
        if not os.access(stage / name, os.X_OK):
            p.error(f"not executable: {name}")
    for name in ("lib", "fonts", "licenses"):
        if not (stage / name).is_dir() or not any((stage / name).iterdir()):
            p.error(f"missing directory or empty: {name}")
    if not (stage / "licenses/THIRD_PARTY_NOTICES.txt").read_text().strip():
        p.error("third-party notices must not be empty; review redistribution licenses")
    if (stage / "model").exists() and not any((stage / "model").rglob("*")):
        p.error("model directory is empty")
    entries = sorted(stage.rglob("*"), key=lambda item: item.relative_to(stage).as_posix())
    if len(entries) > 49999:
        p.error("too many payload members")
    total = 0
    for entry in entries:
        name = entry.relative_to(stage).as_posix()
        if (len(name) > 1024 or len(name.split("/")) > 32 or "\\" in name
                or name.endswith("/") or any(part in ("", ".", "..") for part in name.split("/"))):
            p.error(f"unsupported archive member path: {name}")
        if entry.is_symlink() or not (entry.is_dir() or entry.is_file()):
            p.error(f"links and special files forbidden (copy vetted files into stage): {entry}")
        if entry.is_file():
            total += entry.stat().st_size
    if total > 12 * 1024**3:
        p.error("payload exceeds 12 GiB uncompressed limit")
    name = f"frameyap-{args.version}-{ARCH}.tar.gz"
    target = args.output / name
    sidecar = args.output / (name + ".sha256")
    if target.exists() or sidecar.exists() or target.is_symlink() or sidecar.is_symlink():
        p.error("release output exists; refusing to replace it")
    meta = {"schema": 1, "version": args.version, "arch": ARCH,
            "runtime": "external-authorized-python" if args.external_runtime else "bundled-cpu-python", "model_revision": args.model_revision,
            "includes_model": (stage / "model").is_dir()}
    fd, temp = tempfile.mkstemp(prefix=".frameyap-package-", dir=args.output)
    os.close(fd)
    try:
        with tarfile.open(temp, "w:gz", format=tarfile.PAX_FORMAT, compresslevel=1) as tar:
            payload = (json.dumps(meta, sort_keys=True, indent=2) + "\n").encode()
            info = tarfile.TarInfo("release.json")
            info.size = len(payload)
            info.mode = 0o600
            tar.addfile(info, io.BytesIO(payload))
            for entry in entries:
                tar.add(entry, arcname=entry.relative_to(stage).as_posix(), recursive=False)
        digest = hashlib.sha256()
        with open(temp, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        os.replace(temp, target)
        sidecar.write_text(f"{digest.hexdigest()}  {name}\n")
        print(f"Wrote {target} and {sidecar}")
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


if __name__ == "__main__":
    main()
