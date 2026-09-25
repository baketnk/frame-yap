"""Manifest schema tests: adding a second backend requires no C++ worker changes."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap.model_files import DEFAULT_MANIFEST_DIR, ManifestError, load_backends


class BackendTests(unittest.TestCase):
    def test_redux_manifest_preserves_pinned_artifacts_and_launcher_contract(self):
        redux = load_backends()["redux"]
        self.assertEqual(redux.revision, "fad622f25f303105c20d70e201bcc477c88b620c")
        self.assertEqual({f.path: (f.size, f.sha256) for f in redux.files}, {
            "model.safetensors": (177774490, "78ec25733ee0d0c1586d1346fc86db9d0c2e436e3a8ab1d32a82d1bb8f848d21"),
            "config.json": (12988, "503c653b2e3bb788adbcb04f5abdee532d958686564081baeed133ff10143f6e"),
            "ternary.json": (57970, "1221c6d3ce901ffe09c089da758a8db8b76189f80cff41c5afc244fc61e2051d"),
            "tokenizer.json": (1159960, "bd321b096832a3f270bd3b2a88823957920f1a5c5ada71114a26ea729d0cbe91"),
            "README.md": (8533, "a8b327f983a8b8ff262ff7bead3a791fbed9350632002af8db85ab5cd84cdaa5"),
        })
        self.assertEqual(redux.launcher["protocol"], "frameyap-worker-v1")
        self.assertEqual(redux.launcher["arguments"], ["--model", "{model_dir}", "--threads", "{threads}", "--clip-dir", "{clip_dir}"])
        self.assertIn("CC-BY-4.0", redux.license_id)

    def test_second_backend_and_strict_schema(self):
        fixture = json.loads((DEFAULT_MANIFEST_DIR / "redux.json").read_text())
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            def save(value, name="other.json"):
                (root / name).write_text(json.dumps(value))
            fixture["id"] = "other"
            fixture["display_name"] = "Independent offline test backend"
            fixture["launcher"] = {"type": "executable", "path": "bin/other-worker",
                                   "arguments": ["--model", "{model_dir}", "--clip-dir", "{clip_dir}"],
                                   "protocol": "frameyap-worker-v1"}
            fixture["model"]["files"] = [{"path": "nested/model.bin", "size": 3, "sha256": "a" * 64}]
            save(fixture)
            self.assertEqual(load_backends(root)["other"].files[0].path, "nested/model.bin")
            (root / "redux.json").write_bytes((DEFAULT_MANIFEST_DIR / "redux.json").read_bytes())
            self.assertEqual(list(load_backends(root)), ["other", "redux"])
            for edit in (
                lambda f: f.update(schema=2),
                lambda f: f.update(unexpected="x"),
                lambda f: f["model"]["files"][0].update(path="../secret"),
                lambda f: f["model"]["files"][0].update(size=True),
                lambda f: f["model"]["files"][0].update(sha256="A" * 64),
                lambda f: f["launcher"]["arguments"].append("{unknown}"),
                lambda f: f["launcher"].update(path="/bin/sh"),
                lambda f: f["launcher"].update(protocol="not-the-wire-protocol"),
            ):
                invalid = copy.deepcopy(fixture)
                edit(invalid)
                save(invalid)
                with self.subTest(invalid=invalid), self.assertRaises(ManifestError):
                    load_backends(root)
            save(fixture)
            (root / "other.json").write_text('{"id":"other","id":"other"}')
            with self.assertRaises(ManifestError):
                load_backends(root)
            (root / "other.json").unlink()
            (root / "other.json").symlink_to(DEFAULT_MANIFEST_DIR / "redux.json")
            with self.assertRaises(ManifestError):
                load_backends(root)


if __name__ == "__main__":
    unittest.main()
