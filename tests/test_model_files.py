"""Offline hash/status and CLI tests; fixtures are bytes, not models."""
import contextlib
from dataclasses import replace
import hashlib
import importlib.util
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
from frameyap.model_files import ModelFile, check_file, check_model, load_backends

CLI = ROOT / "scripts" / "model-status.py"


class ModelFileTests(unittest.TestCase):
    def fixture(self, root):
        manifests = root / "manifests"
        manifests.mkdir()
        backend = json.loads((ROOT / "assets/backends/redux.json").read_text())
        backend["model"]["files"] = [{"path": "nested/weights.bin", "size": 7,
                                      "sha256": hashlib.sha256(b"fixture").hexdigest()}]
        (manifests / "redux.json").write_text(json.dumps(backend))
        return manifests, load_backends(manifests)["redux"]

    def cli(self, manifests, *arguments):
        return subprocess.run([sys.executable, str(CLI), "--manifest-dir", str(manifests),
                               "--json", *map(str, arguments)], capture_output=True, text=True, timeout=5)

    def test_status_and_cli_from_same_verifier(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            manifests, backend = self.fixture(root)
            store = root / "models"
            model = store / "redux"
            result = self.cli(manifests, "--list-models", "--model-dir", store)
            self.assertEqual(result.returncode, 0, result.stderr)
            listed = json.loads(result.stdout)
            self.assertEqual(list(listed), ["models", "schema"])
            self.assertEqual(listed["models"][0]["state"], "not_installed")
            self.assertEqual(listed["models"][0]["reason"], "directory_missing")
            self.assertEqual(listed["models"][0]["total_bytes"], 7)
            self.assertEqual(self.cli(manifests, "--check-model", "redux", "--model-dir", model).returncode, 1)
            (model / "nested").mkdir(parents=True)
            weight = model / "nested/weights.bin"
            weight.write_bytes(b"fixture")
            checked = self.cli(manifests, "--check-model", "redux", "--model-dir", model)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertEqual(json.loads(checked.stdout), {"schema": 1, "model": check_model(backend, model)})
            self.assertEqual(check_file(model, backend.files[0]), (None, None))
            weight.write_bytes(b"altered")
            self.assertEqual(check_model(backend, model)["reason"], "hash_mismatch")
            self.assertEqual(json.loads(self.cli(manifests, "--check-model", "redux", "--model-dir", model).stdout)["model"]["state"], "invalid")
            weight.write_bytes(b"short")
            self.assertEqual(check_model(backend, model)["reason"], "size_mismatch")
            weight.unlink()
            self.assertEqual(check_model(backend, model)["state"], "not_installed")
            outside = root / "outside"
            outside.write_bytes(b"fixture")
            weight.symlink_to(outside)
            self.assertEqual(check_model(backend, model)["reason"], "unsafe_file")
            weight.unlink()
            weight.symlink_to(root / "absent")
            self.assertEqual(check_model(backend, model)["reason"], "unsafe_file")
            weight.unlink()
            (model / "nested").rmdir()
            (model / "nested").symlink_to(root)
            self.assertEqual(check_model(backend, model)["reason"], "unsafe_file")

    def test_second_backend_status_without_runtime_changes(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            manifests, _ = self.fixture(root)
            independent = json.loads((manifests / "redux.json").read_text())
            independent["id"] = "independent"
            independent["display_name"] = "Independent backend"
            independent["launcher"] = {"type": "executable", "path": "bin/independent-worker",
                                       "arguments": ["--model", "{model_dir}", "--clip-dir", "{clip_dir}"],
                                       "protocol": "frameyap-worker-v1"}
            independent["model"]["files"] = [{"path": "alternate.bin", "size": 3,
                                               "sha256": hashlib.sha256(b"abc").hexdigest()}]
            (manifests / "independent.json").write_text(json.dumps(independent))
            model_dir = root / "store/independent"
            model_dir.mkdir(parents=True)
            (model_dir / "alternate.bin").write_bytes(b"abc")
            listed = self.cli(manifests, "--list-models", "--model-dir", root / "store")
            self.assertEqual(listed.returncode, 0, listed.stderr)
            models = json.loads(listed.stdout)["models"]
            self.assertEqual([(model["id"], model["state"]) for model in models],
                             [("independent", "installed_verified"), ("redux", "not_installed")])
            self.assertEqual(self.cli(manifests, "--check-model", "independent", "--model-dir", model_dir).returncode, 0)

    def test_explicit_fetch_tool_reuses_shared_verifier_offline(self):
        spec = importlib.util.spec_from_file_location("fetch_model", ROOT / "scripts/fetch-model.py")
        fetch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fetch)
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            _, backend = self.fixture(root)
            backend = replace(backend, files=(ModelFile("weights.bin", 7, hashlib.sha256(b"fixture").hexdigest()),))
            model = root / "local-model"
            # The Redux-only tool uses this small fixture manifest here; all network
            # calls are mocked, never real downloads.
            with patch.object(fetch, "load_backends", return_value={"redux": backend}), \
                 patch.object(fetch.urllib.request, "urlopen", return_value=io.BytesIO(b"fixture")) as urlopen, \
                 patch.object(sys, "argv", ["fetch-model.py", "--destination", str(model)]):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    fetch.main()
                    urlopen.assert_called_once()
                    self.assertEqual(check_model(backend, model)["state"], "installed_verified")
                    fetch.main()
                    urlopen.assert_called_once()
                    (model / "weights.bin").write_bytes(b"changed")
                    with self.assertRaises(SystemExit):
                        fetch.main()
                    urlopen.assert_called_once()

    def test_missing_manifest_unknown_id_and_no_runtime_imports(self):
        with tempfile.TemporaryDirectory() as path:
            manifests, _ = self.fixture(Path(path))
            result = self.cli(manifests, "--check-model", "unknown", "--model-dir", path)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            result = self.cli(Path(path) / "absent", "--list-models")
            self.assertEqual(result.returncode, 2)
            result = self.cli(manifests, "--list-models")
            self.assertEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["models"][0]["state"], "unknown")
            self.assertEqual(json.loads(result.stdout)["models"][0]["reason"], "model_dir_unspecified")
            # This subprocess never imports inference packages even if not installed.
            result = subprocess.run([sys.executable, "-c", "import sys; import frameyap.model_files; "
                                     "assert not {'moondream', 'torch', 'numpy'} & set(sys.modules)"],
                                    env={**os.environ, "PYTHONPATH": str(ROOT / "python")},
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
