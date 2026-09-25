#!/usr/bin/env python3
"""Offline backend inventory and pinned file status; never downloads or imports ASR."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap.model_files import DEFAULT_MANIFEST_DIR, ManifestError, check_model, load_backends


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list-models", action="store_true")
    group.add_argument("--check-model", metavar="ID")
    parser.add_argument("--manifest-dir", type=Path, default=DEFAULT_MANIFEST_DIR)
    parser.add_argument("--model-dir", type=Path,
                        help="direct model directory for --check-model; store root containing ID/ directories for --list-models")
    parser.add_argument("--json", action="store_true", help="stable schema v1 on stdout")
    args = parser.parse_args(argv)
    if args.check_model and args.model_dir is None:
        parser.error("--check-model requires --model-dir")
    try:
        backends = load_backends(args.manifest_dir)
    except ManifestError as error:
        parser.error(str(error))
    if args.check_model:
        if args.check_model not in backends:
            parser.error("unknown backend id")
        result = check_model(backends[args.check_model], args.model_dir)
        payload = {"schema": 1, "model": result}
        code = 0 if result["state"] == "installed_verified" else 1
    else:
        models = []
        for ident, backend in backends.items():
            if args.model_dir is not None:
                models.append(check_model(backend, args.model_dir / ident))
            else:
                result = backend.description()
                result.update(state="unknown", reason="model_dir_unspecified", file=None)
                models.append(result)
        payload = {"schema": 1, "models": models}
        code = 0
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True))
    else:
        for model in payload.get("models", [payload.get("model")]):
            print(f"{model['id']} ({model['display_name']}): {model['state']}" +
                  (f" [{model['reason']}: {model['file']}]" if model['reason'] else ""))
    return code


if __name__ == "__main__":
    sys.exit(main())
