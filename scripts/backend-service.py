#!/usr/bin/env python3
"""Offline status emitter / explicit installer handoff for the native model chooser.

Status is percent-encoded tab-separated records to keep the C++ UI free of a
second manifest/JSON/hash implementation. Installer execution is opt-in ONLY.
"""
import argparse
import hashlib
import os
import re
import stat
from pathlib import Path
import sys
from urllib.parse import quote

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap.model_files import load_backends, check_model


def emit(*fields):
    print("\t".join(quote(str(field), safe="") for field in fields), flush=True)


_DIGEST = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)


def manifest_sha(manifest_dir, ident):
    """Hash bounded raw ID.json bytes, never a reconstructed manifest."""
    path = manifest_dir / (ident + ".json")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > 65536:
            raise ValueError("unsafe backend manifest")
        raw = stream.read(65537)
        after = os.fstat(stream.fileno())
        if len(raw) > 65536 or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("backend manifest changed")
        return hashlib.sha256(raw).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--status", action="store_true")
    operation.add_argument("--install", action="store_true")
    parser.add_argument("--manifest-dir", type=Path, required=True)
    parser.add_argument("--model-store", type=Path, required=True)
    parser.add_argument("--backend")
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--legacy-model", type=Path)
    parser.add_argument("--expected-manifest-sha256")
    args = parser.parse_args(argv)
    if not args.model_store.is_absolute() or not args.manifest_dir.is_absolute():
        parser.error("paths must be absolute")
    if args.status:
        if args.expected_manifest_sha256:
            parser.error("expected manifest applies only to install")
        try:
            # Bracket parsing and model checks with the same raw-byte snapshots.
            # Never emit a consent record for metadata changed during status.
            before = {p.stem: manifest_sha(args.manifest_dir, p.stem)
                      for p in args.manifest_dir.iterdir() if p.suffix == ".json"}
            records = []
            for ident, backend in load_backends(args.manifest_dir).items():
                path = args.model_store / ident
                status = check_model(backend, path)
                if ident == "redux" and args.legacy_model and status["state"] == "not_installed":
                    legacy = check_model(backend, args.legacy_model)
                    if legacy["state"] == "installed_verified":
                        status, path = legacy, args.legacy_model
                records.append(("ST", ident, backend.display_name, status["state"],
                     status["reason"] or "", backend.total_bytes, backend.source,
                     backend.license_id, backend.license_text, backend.attribution, path, before[ident]))
            if before != {p.stem: manifest_sha(args.manifest_dir, p.stem)
                          for p in args.manifest_dir.iterdir() if p.suffix == ".json"}:
                raise ValueError("backend metadata changed during status")
            for record in records:
                emit(*record)
            emit("DONE")
            return 0
        except (ValueError, OSError) as error:
            emit("ERROR", type(error).__name__)
            return 1
    # No implicit installation: requires explicit --install AND backend and caller
    # consent. The exec makes the installer the owned child (no hidden grandchild).
    if (not args.backend or not args.installer or not args.expected_manifest_sha256 or
            not _DIGEST.fullmatch(args.expected_manifest_sha256)):
        parser.error("backend, installer and 64-character consent fingerprint required")
    try:
        backends = load_backends(args.manifest_dir)
        if args.backend not in backends or manifest_sha(args.manifest_dir, args.backend) != args.expected_manifest_sha256:
            parser.error("selected backend manifest changed since consent")
    except (OSError, ValueError) as error:
        parser.error(f"selected backend manifest unavailable: {type(error).__name__}")
    if not args.installer.is_file() or args.installer.is_symlink():
        parser.error("installer missing or unsafe")
    dest = args.model_store / args.backend
    os.execv("/bin/sh", ["sh", str(args.installer), "--install-model", "--backend", args.backend,
                          "--model-dir", str(dest), "--expected-manifest-sha256",
                          args.expected_manifest_sha256, "--yes", "--json"])


if __name__ == "__main__":
    sys.exit(main())
