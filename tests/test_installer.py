"""Offline installer/packager checks: temporary HOME only, no hardware/network."""
import contextlib
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import shutil
import pty
import select
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("install_payload", REPO / "scripts/install_payload.py")
installer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.home.mkdir()
        self.data = self.base / "data"
        self.output = self.base / "out"
        self.output.mkdir()
        self.stage = self.base / "stage"
        for name in ("bin/frameyap", "runtime/bin/python3", "runtime/bin/helper",
                     "python/frameyap/worker.py", "assets/actions.json", "fonts/font.ttf", "lib/libtest.so",
                     "licenses/THIRD_PARTY_NOTICES.txt", "model/weights.bin"):
            path = self.stage / name
            path.parent.mkdir(exist_ok=True, parents=True)
            path.write_bytes((name + " fixture\n").encode())
        for name in ("bin/frameyap", "runtime/bin/python3", "runtime/bin/helper", "lib/libtest.so"):
            (self.stage / name).chmod(0o755)
        shutil.copyfile(REPO / "install.sh", self.stage / "bin/install.sh")
        (self.stage / "bin/install.sh").chmod(0o755)
        (self.stage / "scripts").mkdir()
        (self.stage / "scripts/backend-service.py").write_text("# offline fixture service\n")
        self.env = patch.dict(os.environ, {"HOME": str(self.home), "XDG_DATA_HOME": str(self.data),
                                         "XDG_CONFIG_HOME": str(self.home / ".config")})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.host = patch.object(installer, "check_host")
        self.host.start()
        self.addCleanup(self.host.stop)

    def package(self, version, *extra):
        subprocess.run([sys.executable, str(REPO / "scripts/package-release.py"), "--stage", str(self.stage),
                        "--output", str(self.output), "--arch", "linux-aarch64", "--version", version,
                        "--model-revision", "test-revision", *extra], check=True, capture_output=True)
        archive = self.output / installer.release_name(version)
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

    def install(self, version, archive, sha, *extra):
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--archive", str(archive), "--sha256", sha, "--version", version, *extra])

    def test_embedded_installer_is_current(self):
        wrapper = (REPO / "install.sh").read_text()
        self.assertEqual(wrapper.split("<<'PY'\n", 1)[1].removesuffix("\nPY\n"),
                         (REPO / "scripts/install_payload.py").read_text())

    def test_install_idempotence_upgrade_rollback_uninstall(self):
        a1, h1 = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a1, h1)
        root = self.data / "frameyap"
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241530")
        launcher = self.home / ".local/bin/frameyap"
        self.assertIn("--run", launcher.read_text())
        self.assertNotIn("--font", launcher.read_text())  # run/check modes honor config font
        config = self.home / ".config/frameyap/config.json"
        self.assertEqual(json.loads(config.read_text()), installer.CONFIG_DEFAULTS)
        self.assertIs(json.loads(config.read_text())["advanced_debug"], False)
        self.assertIs(json.loads(config.read_text())["auto_insert"], False)
        self.assertIs(json.loads(config.read_text())["close_mic_when_idle"], False)
        self.assertEqual(json.loads(config.read_text())["backend"], "redux")
        self.assertIs(json.loads(config.read_text())["lock_layout"], False)
        self.assertEqual(list(config.parent.glob("config.json.backup-*")), [])
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", launcher.read_text())
        self.assertTrue(os.access(root / "versions/0.1.202609241530/runtime/bin/helper", os.X_OK))
        self.assertTrue(os.access(root / "versions/0.1.202609241530/lib/libtest.so", os.X_OK))
        managed_installer = root / "current/bin/install.sh"
        self.assertEqual(managed_installer.read_bytes(), (REPO / "install.sh").read_bytes())
        self.assertTrue(os.access(managed_installer, os.X_OK))
        self.assertTrue((root / "current/scripts/backend-service.py").is_file())
        manifest = json.loads((root / "frameyap.vrmanifest").read_text())
        self.assertEqual(manifest["applications"][0]["app_key"], "local.frameyap.overlay")
        self.assertEqual(manifest["applications"][0]["binary_path_linux"], str(launcher))
        self.assertEqual(manifest["applications"][0]["binary_path_linux_arm"], str(launcher))
        self.assertNotIn("binary_path", manifest["applications"][0])
        desktop = self.data / "applications/frameyap.desktop"
        self.assertIn(f'Exec="{launcher}"', desktop.read_text())
        self.assertIn("Terminal=false", desktop.read_text())
        (root / "config-untouched").write_text("keep")
        self.install("0.1.202609241530", a1, h1)
        (self.stage / "bin/frameyap").write_text("next binary")
        a2, h2 = self.package("0.1.202609241531")
        self.install("0.1.202609241531", a2, h2)
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241531")
        self.assertEqual(os.readlink(root / "previous"), "versions/0.1.202609241530")
        (root / "frameyap.vrmanifest").write_text("foreign")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            installer.main(["--rollback"])
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241531")
        (root / "frameyap.vrmanifest").unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--rollback"])
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241530")
        self.assertEqual(os.readlink(root / "previous"), "versions/0.1.202609241531")
        with self.assertRaises(installer.UsageError):
            installer.main(["--uninstall"])
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--uninstall", "--unregistered"])
        self.assertEqual((root / "config-untouched").read_text(), "keep")
        self.assertTrue((root / "saved-models/0.1.202609241530/weights.bin").exists())
        self.assertTrue((root / "saved-models/0.1.202609241531/weights.bin").exists())
        self.assertFalse(launcher.exists())
        self.assertFalse(desktop.exists())

    def test_previous_legacy_version_remains_rollback_selectable(self):
        first, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", first, digest)
        root = self.data / "frameyap"
        original = root / "versions/0.1.202609241530"
        legacy = root / "versions/v0.1.0-poc-local"
        original.rename(legacy)
        meta = json.loads((legacy / "release.json").read_text())
        meta["version"] = legacy.name  # emulate older already-installed metadata
        (legacy / "release.json").write_text(json.dumps(meta))
        (root / "current").unlink()
        (root / "current").symlink_to("versions/" + legacy.name)
        second, digest2 = self.package("0.1.202609241531")
        self.install("0.1.202609241531", second, digest2)
        self.assertEqual(os.readlink(root / "previous"), "versions/" + legacy.name)
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--rollback"])
        self.assertEqual(os.readlink(root / "current"), "versions/" + legacy.name)

    def test_config_install_repairs_and_preserves_original(self):
        archive, digest = self.package("0.1.202609241530")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        original = b'{"font":"/system/face.ttf","theme":{"ink":"#F1f2F3"},"buttons":{"ptt":"/user/hand/left/input/y"}}\n'
        config.write_bytes(original)
        self.install("0.1.202609241530", archive, digest)
        fixed = json.loads(config.read_text())
        self.assertEqual(fixed["font"], "/system/face.ttf")
        self.assertEqual(fixed["theme"]["ink"], "#F1f2F3")
        self.assertEqual(fixed["buttons"]["ptt"], "/user/hand/left/input/y")
        self.assertEqual(fixed["theme"]["card"], installer.CONFIG_DEFAULTS["theme"]["card"])
        self.assertEqual(fixed["buttons"]["cancel"], "/user/hand/right/input/b")
        self.assertEqual(fixed["input_priority"], "normal")
        self.assertIs(fixed["advanced_debug"], False)
        self.assertIs(fixed["auto_insert"], False)
        self.assertIs(fixed["lock_layout"], False)
        self.assertIs(fixed["clock_24h"], False)
        self.assertEqual(fixed["date_format"], "mdy")
        self.assertEqual(fixed["quick_inputs"], ["/new", "/questions", "/help"])
        self.assertEqual(fixed["buttons"]["quick_chat"], "/user/hand/right/input/y")
        self.assertEqual(fixed["wrist"], installer.CONFIG_DEFAULTS["wrist"])
        legacy = installer.normalized_config({"buttons": {"enter": "/user/hand/right/input/y"}})
        self.assertEqual(legacy["buttons"]["enter"], "")
        self.assertEqual(legacy["buttons"]["quick_chat"], "/user/hand/right/input/y")
        self.assertEqual(installer.normalized_config({"quick_inputs": ["oops\n"]})["quick_inputs"],
                         ["/new", "/questions", "/help"])
        backups = list(config.parent.glob("config.json.backup-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), original)
        fixed["input_priority"] = "experimental"
        fixed["advanced_debug"] = True
        fixed["auto_insert"] = True
        fixed["close_mic_when_idle"] = True
        fixed["backend"] = "custom_v2-1"
        fixed["lock_layout"] = True
        fixed["clock_24h"] = True
        fixed["date_format"] = "iso"
        fixed["buttons"]["enter"] = ""  # intentional disabling survives upgrades
        fixed["quick_inputs"] = ["/new", "/questions", "hello there"]
        fixed["wrist"]["y"] = 0.2
        compact = json.dumps(fixed, separators=(",", ":")).encode()
        config.write_bytes(compact)
        self.install("0.1.202609241530", archive, digest)
        self.assertEqual(config.read_bytes(), compact)
        self.assertIs(json.loads(config.read_text())["advanced_debug"], True)
        self.assertIs(json.loads(config.read_text())["auto_insert"], True)
        self.assertIs(json.loads(config.read_text())["close_mic_when_idle"], True)
        self.assertEqual(json.loads(config.read_text())["backend"], "custom_v2-1")
        self.assertIs(json.loads(config.read_text())["lock_layout"], True)
        self.assertIs(json.loads(config.read_text())["clock_24h"], True)
        self.assertEqual(json.loads(config.read_text())["date_format"], "iso")
        self.assertEqual(len(list(config.parent.glob("config.json.backup-*"))), 1)
        fixed["advanced_debug"] = False
        compact_off = json.dumps(fixed, separators=(",", ":")).encode()
        config.write_bytes(compact_off)
        self.install("0.1.202609241530", archive, digest)
        self.assertEqual(config.read_bytes(), compact_off)
        self.assertEqual(len(list(config.parent.glob("config.json.backup-*"))), 1)
        original = b'{"font":"/system/face.ttf","input_priority":"highest","wrist":{"x":0.04,"y":true,"width":100,"obsolete":4},"theme":{"ink":"bad","retired":"#123456"},"buttons":{"ptt":"/user/hand/left/input/grip"},"old_option":4}'
        config.write_bytes(original)
        self.install("0.1.202609241530", archive, digest)
        fixed = json.loads(config.read_text())
        self.assertEqual(fixed["font"], "/system/face.ttf")
        self.assertEqual(fixed["theme"]["ink"], installer.CONFIG_DEFAULTS["theme"]["ink"])
        self.assertEqual(fixed["buttons"], installer.CONFIG_DEFAULTS["buttons"])  # colliding paths reset
        self.assertEqual(fixed["quick_inputs"], installer.CONFIG_DEFAULTS["quick_inputs"])
        self.assertEqual(fixed["input_priority"], "normal")
        self.assertIs(fixed["advanced_debug"], False)
        self.assertIs(fixed["auto_insert"], False)
        self.assertIs(fixed["lock_layout"], False)
        self.assertEqual(fixed["wrist"]["x"], 0.04)
        self.assertEqual(fixed["wrist"]["y"], 0.18)
        self.assertEqual(fixed["wrist"]["width"], 0.30)
        self.assertNotIn("obsolete", fixed["wrist"])
        self.assertNotIn("old_option", fixed)
        self.assertEqual(sorted(p.read_bytes() for p in config.parent.glob("config.json.backup-*")), sorted([backups[0].read_bytes(), original]))
        original = b'{"font":"one","font":"two"'
        config.write_bytes(original)
        self.install("0.1.202609241530", archive, digest)
        self.assertEqual(json.loads(config.read_text()), installer.CONFIG_DEFAULTS)
        self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])
        config.write_text(json.dumps({"font": "/" + "x" * 3800}))
        self.install("0.1.202609241530", archive, digest)
        self.assertEqual(json.loads(config.read_text())["font"], "")
        self.assertTrue(all(len(p.read_bytes()) > 0 for p in config.parent.glob("config.json.backup-*")))
        config.write_bytes(b"x" * 65537)
        with self.assertRaisesRegex(ValueError, "too large"):
            self.install("0.1.202609241530", archive, digest)
        self.assertEqual(config.stat().st_size, 65537)
        config.unlink()
        config.symlink_to(self.stage / "model/weights.bin")
        with self.assertRaisesRegex(ValueError, "foreign config path"):
            self.install("0.1.202609241530", archive, digest)
        self.assertTrue(config.is_symlink())

    def test_debug_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("0.1.202609241530")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"advanced_debug": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("0.1.202609241530", archive, digest)
            self.assertIs(json.loads(config.read_text())["advanced_debug"], False)
            self.assertEqual(json.loads(config.read_text())["font"], "/custom/font.ttf")
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_auto_insert_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("0.1.202609241530")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"auto_insert": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("0.1.202609241530", archive, digest)
            self.assertIs(json.loads(config.read_text())["auto_insert"], False)
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_lock_layout_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("0.1.202609241530")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"lock_layout": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("0.1.202609241530", archive, digest)
            fixed = json.loads(config.read_text())
            self.assertIs(fixed["lock_layout"], False)
            self.assertEqual(fixed["font"], "/custom/font.ttf")
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_new_config_fields_repair_only_invalid_values(self):
        archive, digest = self.package("0.1.202609241530")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"close_mic_when_idle": invalid, "backend": "../unsafe"}).encode()
            config.write_bytes(original)
            self.install("0.1.202609241530", archive, digest)
            fixed = json.loads(config.read_text())
            self.assertIs(fixed["close_mic_when_idle"], False)
            self.assertEqual(fixed["backend"], "redux")
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])
        for value in ("a", "a" + "9" * 47, "custom_backend-2"):
            self.assertEqual(installer.normalized_config({"backend": value})["backend"], value)
        for invalid in ("", "9bad", "a" * 49, "UPPER", "a/b", 12):
            self.assertEqual(installer.normalized_config({"backend": invalid})["backend"], "redux")

    def test_digest_and_same_version_mismatch_leave_previous(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        root = self.data / "frameyap"
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.install("0.1.202609241530", a, "0" * 64)
        a.unlink()
        (self.output / (a.name + ".sha256")).unlink()
        (self.stage / "bin/frameyap").write_text("changed")
        a, h2 = self.package("0.1.202609241530")
        with self.assertRaisesRegex(ValueError, "different archive digest"):
            self.install("0.1.202609241530", a, h2)
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241530")

    def test_without_model_lock_and_foreign_launcher(self):
        a, h = self.package("0.1.202609241530")
        lock = self.data / "frameyap/.lock"
        lock.parent.mkdir(parents=True)
        with lock.open("a+b") as fd:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "running"):
                self.install("0.1.202609241530", a, h)
        self.install("0.1.202609241530", a, h, "--without-model")
        root = self.data / "frameyap"
        self.assertFalse((root / "versions/0.1.202609241530/model").exists())
        self.assertTrue((self.stage / "model/weights.bin").exists())
        self.assertEqual(json.loads((root / "versions/0.1.202609241530/.install-options.json").read_text()),
                         {"without_model": True})
        self.install("0.1.202609241530", a, h, "--without-model")
        self.assertFalse((root / "versions/0.1.202609241530/model").exists())
        with self.assertRaisesRegex(ValueError, "different --without-model choice"):
            self.install("0.1.202609241530", a, h)
        self.assertFalse((root / "versions/0.1.202609241530/model").exists())
        (root / "frameyap.vrmanifest").unlink()
        launcher = self.home / ".local/bin/frameyap"
        launcher.unlink()
        self.install("0.1.202609241530", a, h, "--without-model")
        self.assertTrue(launcher.exists())
        self.assertTrue((root / "frameyap.vrmanifest").exists())
        (root / "frameyap.vrmanifest").write_text("foreign")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("0.1.202609241530", a, h, "--without-model")
        (root / "frameyap.vrmanifest").unlink()
        self.install("0.1.202609241530", a, h, "--without-model")
        (self.home / ".local/bin/frameyap").write_text("#!/bin/sh\n" + installer.MARKER + "echo foreign\n")
        (self.stage / "bin/frameyap").write_text("new")
        a2, h2 = self.package("0.1.202609241531")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("0.1.202609241531", a2, h2)
        self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/0.1.202609241530")
        self.assertFalse((self.data / "frameyap/versions/0.1.202609241531").exists())

    def test_foreign_desktop_entry_is_preserved(self):
        a, h = self.package("0.1.202609241530")
        desktop = self.data / "applications/frameyap.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text("foreign shortcut\n")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("0.1.202609241530", a, h)
        self.assertEqual(desktop.read_text(), "foreign shortcut\n")
        self.assertFalse((self.data / "frameyap/current").exists())

    def test_desktop_exec_escapes_field_codes(self):
        entry = installer.desired_desktop(Path('/home/steam%user/.local/bin/frameyap')).decode()
        self.assertIn('Exec="/home/steam%%user/.local/bin/frameyap"', entry)

    def test_traversal_and_link_archives_rejected(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        for badname, kind in (("../outside", "file"), ("bin/escape", "symlink"),
                              ("/absolute", "file"), ("bin/escape", "hardlink")):
            with self.subTest(badname=badname, kind=kind):
                bad = self.base / "bad.tar.gz"
                with tarfile.open(bad, "w:gz") as tar:
                    info = tarfile.TarInfo(badname)
                    if kind != "file":
                        info.type = tarfile.SYMTYPE if kind == "symlink" else tarfile.LNKTYPE
                        info.linkname = "../../outside"
                        tar.addfile(info)
                    else:
                        info.size = 1
                        tar.addfile(info, io.BytesIO(b"x"))
                sha = hashlib.sha256(bad.read_bytes()).hexdigest()
                with self.assertRaisesRegex(ValueError, "unsafe archive|forbidden"):
                    self.install("0.1.202609241531", bad, sha)
                self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/0.1.202609241530")
                self.assertFalse((self.base / "outside").exists())

    def test_unexpected_root_entries_and_oversized_metadata_rejected(self):
        for name, size in (("release.json/child", 1), ("bin", 1), ("release.json", 8193)):
            with self.subTest(name=name, size=size):
                bad = self.base / "bad.tar.gz"
                with tarfile.open(bad, "w:gz") as tar:
                    info = tarfile.TarInfo(name)
                    info.size = size
                    tar.addfile(info, io.BytesIO(b"x" * size))
                sha = hashlib.sha256(bad.read_bytes()).hexdigest()
                with self.assertRaisesRegex(ValueError, "unexpected archive path|oversized release metadata"):
                    self.install("0.1.202609241530", bad, sha)

    def test_uninstall_refuses_untracked_files(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        root = self.data / "frameyap"
        extra = root / "versions/0.1.202609241530/user.txt"
        extra.write_text("preserve")
        with self.assertRaisesRegex(ValueError, "untracked files"):
            with contextlib.redirect_stdout(io.StringIO()):
                installer.main(["--uninstall", "--unregistered"])
        self.assertTrue(extra.exists())
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241530")

    def test_release_metadata_mismatch_retains_current(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        a2, h2 = self.package("0.1.202609241531")
        with self.assertRaisesRegex(ValueError, "metadata version/architecture/schema mismatch"):
            self.install("0.1.202609241532", a2, h2)
        self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/0.1.202609241530")

    def test_launcher_defaults_run_but_forwards_registration(self):
        path = self.stage / "bin/frameyap"
        path.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\nprintf "ENV:%s\\n" "${PYTHONDONTWRITEBYTECODE:-}"\n')
        path.chmod(0o755)
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        launcher = self.home / ".local/bin/frameyap"
        reg = subprocess.run([str(launcher), "--register", "test-manifest"], capture_output=True, text=True, check=True)
        self.assertEqual(reg.stdout.splitlines(), ["--register", "test-manifest", "ENV:1"])
        run = subprocess.run([str(launcher)], capture_output=True, text=True, check=True,
                             env={**os.environ, "GAMESCOPE_SOCKET": "fixture-socket"})
        self.assertEqual(run.stdout.splitlines()[0], "--run")
        self.assertIn("--assets", run.stdout)
        self.assertIn("--socket\nfixture-socket\n", run.stdout)
        self.assertIn("ENV:1", run.stdout)
        self.assertEqual(json.loads((self.data / "frameyap/versions/0.1.202609241530/.install-options.json").read_text()),
                         {"without_model": False})
        with self.assertRaisesRegex(ValueError, "different --without-model choice"):
            self.install("0.1.202609241530", a, h, "--without-model")
        self.assertTrue((self.data / "frameyap/versions/0.1.202609241530/model/weights.bin").exists())

    def test_no_model_reinstall_preserves_provisioned_model_and_uninstall(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h, "--without-model")
        root = self.data / "frameyap"
        model = root / "versions/0.1.202609241530/model"
        model.mkdir()
        (model / "provided.bin").write_text("user-provided")
        self.install("0.1.202609241530", a, h, "--without-model")
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--uninstall", "--unregistered"])
        self.assertEqual((root / "saved-models/0.1.202609241530/provided.bin").read_text(), "user-provided")

    def test_version_switch_rejects_untracked_installed_files(self):
        a, h = self.package("0.1.202609241530")
        self.install("0.1.202609241530", a, h)
        (self.data / "frameyap/versions/0.1.202609241530/untracked").write_text("keep")
        a2, h2 = self.package("0.1.202609241531")
        with self.assertRaisesRegex(ValueError, "untracked files"):
            self.install("0.1.202609241530", a, h)
        # Upgrade does not delete the old directory, but uninstall must still refuse it.
        self.install("0.1.202609241531", a2, h2)
        with self.assertRaisesRegex(ValueError, "untracked files"):
            installer.main(["--uninstall", "--unregistered"])

    def test_external_authorized_runtime_is_explicit(self):
        import shutil
        shutil.rmtree(self.stage / "runtime")
        native = self.stage / "bin/frameyap"
        native.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        native.chmod(0o755)
        a, h = self.package("0.1.202609241533", "--external-runtime")
        self.install("0.1.202609241533", a, h)
        root = self.data / "frameyap/current"
        self.assertEqual(json.loads((root / "release.json").read_text())["runtime"], "external-authorized-python")
        self.assertFalse((root / "runtime").exists())
        launcher = self.home / ".local/bin/frameyap"
        run = subprocess.run([str(launcher)], capture_output=True, text=True, check=True,
                             env={**os.environ, "FRAMEYAP_PYTHON": "/authorized/python", "FRAMEYAP_MODEL": "/local/model"})
        self.assertIn("--python\n/authorized/python\n--model\n/local/model\n", run.stdout)
        probe = subprocess.run([str(launcher), "--check-controls", "--head"], capture_output=True, text=True, check=True)
        self.assertTrue(probe.stdout.startswith("--check-controls\n--assets\n"))
        self.assertNotIn("--python", probe.stdout)

    def test_external_runtime_paths_config_and_legacy_launcher_migration(self):
        import shutil
        shutil.rmtree(self.stage / "runtime")
        native = self.stage / "bin/frameyap"
        native.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        native.chmod(0o755)
        a, h = self.package("0.1.202609241533", "--external-runtime")
        self.install("0.1.202609241533", a, h)
        root = self.data / "frameyap"
        launcher = self.home / ".local/bin/frameyap"
        # A pre-config release launcher may be upgraded only when its exact
        # bytes match the managed legacy template, not just its marker.
        launcher.write_bytes(installer.desired_launcher(root, legacy=True))
        self.install("0.1.202609241533", a, h)
        self.assertEqual(launcher.read_bytes(), installer.desired_launcher(root))
        config = self.home / ".config/frameyap/paths.conf"
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text("# Literal paths, not shell code\npython=/opt/approved python/bin/python3\n"
                          "model=/opt/$(printf not-executed)/local model\n")
        run = subprocess.run([str(launcher)], capture_output=True, text=True, check=True)
        self.assertIn("--python\n/opt/approved python/bin/python3\n"
                      "--model\n/opt/$(printf not-executed)/local model\n", run.stdout)
        override = subprocess.run([str(launcher)], capture_output=True, text=True, check=True,
                                  env={**os.environ, "FRAMEYAP_PYTHON": "/other/python"})
        self.assertIn("--python\n/other/python\n--model\n/opt/$(printf not-executed)/local model\n", override.stdout)
        config.unlink()
        config.symlink_to(self.stage / "model/weights.bin")
        symlinked = subprocess.run([str(launcher)], capture_output=True, text=True, check=True)
        self.assertIn(f"--model\n{root}/current/model\n", symlinked.stdout)
        # A user-modified launcher must remain protected, even with the marker.
        launcher.write_bytes(installer.desired_launcher(root, legacy=True) + b"# changed\n")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("0.1.202609241533", a, h)

    def test_native_stage_notices_credit_system_freetype_without_bundling(self):
        spec = importlib.util.spec_from_file_location("stage_native", REPO / "scripts/stage-native.py")
        stage_native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stage_native)
        build = self.base / "native-build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text("FRAMEYAP_NATIVE:BOOL=ON\n")
        dest = self.base / "native-stage"
        args = ["stage-native.py", "--build", str(build), "--destination", str(dest)]
        for flag in ("openvr-library", "openvr-license", "sdl-library", "sdl-license"):
            args += ["--" + flag, str(self.stage / "lib/libtest.so")]
        def fake_install(*_args, **_kwargs):
            (dest / "bin").mkdir(parents=True)
        with patch.object(stage_native.subprocess, "run", side_effect=fake_install), \
             patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
            stage_native.main()
        notice = (dest / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
        self.assertIn("This software is based in part on the work of the FreeType Team.", notice)
        self.assertIn("FreeType is a system dynamic library, not bundled", notice)
        self.assertIn("Bundled libraries: Valve OpenVR and unmodified SDL3", notice)
        self.assertFalse(any(dest.glob("lib/*FreeType*")))

    def test_package_rejects_symlink(self):
        (self.stage / "lib/link.so").symlink_to("libtest.so")
        result = subprocess.run([sys.executable, str(REPO / "scripts/package-release.py"), "--stage", str(self.stage),
            "--output", str(self.output), "--arch", "linux-aarch64", "--version", "0.1.202609241530",
            "--model-revision", "test"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("links and special files forbidden", result.stderr)

    def test_numeric_utc_release_version_roundtrip(self):
        for version in ("0.1.202609241627", "2.13.202609241628", "12.0.202609241629"):
            with self.subTest(version=version):
                archive, digest = self.package(version)
                self.assertEqual(archive.name, f"frameyap-{version}-linux-aarch64.tar.gz")
                self.install(version, archive, digest)
                root = self.data / "frameyap"
                self.assertEqual(os.readlink(root / "current"), f"versions/{version}")
                self.assertEqual(json.loads((root / "current/release.json").read_text())["version"], version)

    def test_autolaunch_is_opt_in_and_registration_is_after_lock(self):
        native = self.stage / "bin/frameyap"
        native.write_text('#!/usr/bin/env python3\n'
                          'import fcntl, os, pathlib, sys\n'
                          'assert sys.argv[1] == "--register"\n'
                          'pathlib.Path(os.environ["HOME"], "register-args").write_text("\\n".join(sys.argv[1:])+"\\n")\n'
                          'with pathlib.Path(os.environ["XDG_DATA_HOME"], "frameyap/.lock").open("a+b") as f:\n'
                          '    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)\n')
        native.chmod(0o755)
        archive, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", archive, digest)
        record = self.home / "register-args"
        self.assertFalse(record.exists())
        self.install("0.1.202609241530", archive, digest, "--autolaunch")
        self.assertEqual(record.read_text().splitlines()[-1], "--autostart")
        self.install("0.1.202609241530", archive, digest, "--no-autolaunch")
        self.assertEqual(record.read_text().splitlines(),
                         ["--register", str(self.data / "frameyap/frameyap.vrmanifest")])

    def test_numeric_package_rejects_git_suffix_bad_date_and_tag(self):
        for version in ("v0.1.202609241530", "0.1.202609241530-gabc123", "0.1.202613241530",
                        "01.1.202609241530", "1.01.202609241530", "2.13.202613241530"):
            with self.subTest(version=version):
                result = subprocess.run([sys.executable, str(REPO / "scripts/package-release.py"),
                    "--stage", str(self.stage), "--output", str(self.output), "--arch", "linux-aarch64",
                    "--version", version, "--model-revision", "fixture"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                with self.assertRaisesRegex(ValueError, "version|date/time"):
                    installer.check_version(version)

    def test_piped_wrapper_json_usage_error_never_reads_stdin(self):
        result = subprocess.run(["sh", str(REPO / "install.sh"), "--mode", "source", "--json"],
                                input="unused and unread", text=True, capture_output=True,
                                timeout=8, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["code"], "usage")
        self.assertEqual(result.stderr, "")
        self.assertFalse((self.data / "frameyap").exists())

    def test_piped_input_with_tty_output_does_not_prompt(self):
        master, slave = pty.openpty()
        try:
            child = subprocess.Popen(["sh", str(REPO / "install.sh")], stdin=subprocess.PIPE,
                                     stdout=slave, stderr=slave,
                                     env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            os.close(slave)
            slave = -1
            output = bytearray()
            try:
                child.stdin.write(b"source\n")
                child.stdin.close()
                while child.poll() is None:
                    ready, _, _ = select.select([master], [], [], 8)
                    self.assertTrue(ready, "piped installer did not exit")
                    try:
                        output.extend(os.read(master, 8192))
                    except OSError:
                        break
                self.assertEqual(child.wait(timeout=8), 2)
                self.assertNotIn(b"Mode [binary/source]:", output)
                self.assertFalse((self.data / "frameyap").exists())
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=8)
        finally:
            os.close(master)
            if slave >= 0:
                os.close(slave)

    def test_attended_shell_wrapper_reads_the_actual_tty(self):
        master, slave = pty.openpty()
        try:
            child = subprocess.Popen(["sh", str(REPO / "install.sh")], stdin=slave,
                                     stdout=slave, stderr=slave,
                                     env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            os.close(slave)
            slave = -1
            output = bytearray()
            try:
                os.write(master, b"not-a-mode\n")
                while child.poll() is None:
                    ready, _, _ = select.select([master], [], [], 8)
                    self.assertTrue(ready, "installer did not return from terminal input")
                    try:
                        output.extend(os.read(master, 8192))
                    except OSError:
                        break
                self.assertEqual(child.wait(timeout=8), 2)
                self.assertIn(b"Mode [binary/source]:", output)
                self.assertIn(b"choose binary or source", output)
                self.assertFalse((self.data / "frameyap").exists())
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=8)
        finally:
            os.close(master)
            if slave >= 0:
                os.close(slave)

    def test_plan_and_json_errors_do_not_install_or_prompt(self):
        root = self.data / "frameyap"
        response = io.StringIO()
        with contextlib.redirect_stdout(response):
            self.assertEqual(installer.cli(["--mode", "binary", "--version", "0.1.202609241530",
                                            "--print-plan", "--json"]), 0)
        result = json.loads(response.getvalue())
        self.assertEqual(result["event"], "plan")
        self.assertEqual(result["tag"], "v0.1.202609241530")
        self.assertTrue(result["network"])
        self.assertFalse(root.exists())
        response = io.StringIO()
        with contextlib.redirect_stdout(response):
            self.assertEqual(installer.cli(["--mode", "source", "--json"]), 2)
        error = json.loads(response.getvalue())
        self.assertEqual((error["code"], error["exit_code"]), ("usage", 2))
        self.assertFalse(root.exists())
        response = io.StringIO()
        with contextlib.redirect_stdout(response):
            self.assertEqual(installer.cli(["--version", "0.1.202609241530", "--json"]), 1)
        self.assertIn("--yes", json.loads(response.getvalue())["message"])
        self.assertFalse(root.exists())

    def test_json_plan_rejects_invalid_installed_manifest_without_mutation(self):
        manifests = self.stage / "assets/backends"
        manifests.mkdir()
        shutil.copyfile(REPO / "python/frameyap/model_files.py",
                        self.stage / "python/frameyap/model_files.py")
        (manifests / "redux.json").write_text('{"broken": true}')
        archive, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", archive, digest, "--without-model")
        root = self.data / "frameyap"
        original = sorted(p.relative_to(root).as_posix() for p in root.rglob("*"))
        response = io.StringIO()
        with contextlib.redirect_stdout(response):
            self.assertEqual(installer.cli(["--print-plan", "--install-model", "--backend", "redux", "--json"]), 1)
        self.assertEqual(json.loads(response.getvalue())["code"], "operation_failed")
        self.assertEqual(original, sorted(p.relative_to(root).as_posix() for p in root.rglob("*")))

    def test_running_app_and_model_provisioning_locks_preserve_session(self):
        first, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", first, digest)
        root = self.data / "frameyap"
        next_version = "0.1.202609241531"
        second, digest2 = self.package(next_version)
        with (root / ".model.lock").open("a+b") as provision_lock:
            fcntl.flock(provision_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "model provisioning is running"):
                self.install(next_version, second, digest2)
        with (root / ".lock").open("a+b") as app_lock:
            fcntl.flock(app_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "application is running"):
                self.install(next_version, second, digest2)
        self.assertEqual(os.readlink(root / "current"), "versions/0.1.202609241530")
        self.assertFalse((root / "versions" / next_version).exists())

    def test_source_preflight_refuses_missing_tools_before_build(self):
        sources = self.base / "sources"
        (sources / "scripts").mkdir(parents=True)
        for name in ("CMakeLists.txt", "scripts/install-preflight.sh", "scripts/stage-native.py", "scripts/package-release.py"):
            (sources / name).write_text("fixture")
        sdk = self.base / "sdk"
        (sdk / "headers").mkdir(parents=True)
        (sdk / "headers/openvr.h").write_text("fixture")
        argv = ["--mode", "source", "--source", str(sources), "--openvr-root", str(sdk),
                "--version", "0.1.202609241530"]
        for flag in ("openvr-library", "openvr-license", "sdl-library", "sdl-license"):
            argv += ["--" + flag, str(self.stage / "bin/frameyap")]
        self.assertFalse(installer.plan(installer.resolve_args(argv))["network"])
        with patch.object(installer.shutil, "which", return_value=None), patch.object(installer.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "source prerequisite missing: cmake"):
                installer.main(argv)
            run.assert_not_called()
        self.assertFalse((self.data / "frameyap/current").exists())
        # A local source tree must pass its own read-only shell preflight; do
        # not create an install root or start a build when it reports failure.
        with patch.object(installer.shutil, "which", return_value="/mock/tool"), \
             patch.object(installer.subprocess, "run", return_value=SimpleNamespace(returncode=1, stderr="missing Vulkan")) as run:
            with self.assertRaisesRegex(ValueError, "source preflight failed.*missing Vulkan"):
                installer.main(argv)
            self.assertEqual(run.call_args.args[0], ["sh", str(sources / "scripts/install-preflight.sh"), "--source"])
        self.assertFalse((self.data / "frameyap").exists())

    def test_source_mode_packages_local_build_and_keeps_rollback(self):
        previous, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", previous, digest)
        candidate, _ = self.package("0.1.202609241531")
        sources = self.base / "source"
        (sources / "scripts").mkdir(parents=True)
        for name in ("CMakeLists.txt", "scripts/install-preflight.sh", "scripts/stage-native.py", "scripts/package-release.py"):
            (sources / name).write_text("fixture")
        sdk = self.base / "sdk"
        (sdk / "headers").mkdir(parents=True)
        (sdk / "headers/openvr.h").write_text("fixture")
        argv = ["--mode", "source", "--source", str(sources), "--openvr-root", str(sdk),
                "--version", "0.1.202609241531"]
        for flag in ("openvr-library", "openvr-license", "sdl-library", "sdl-license"):
            argv += ["--" + flag, str(self.stage / "bin/frameyap")]
        (sources / "assets/backends").mkdir(parents=True)
        source_manifest = json.loads((REPO / "assets/backends/redux.json").read_text())
        source_manifest["model"]["revision"] = "a" * 40
        (sources / "assets/backends/redux.json").write_text(json.dumps(source_manifest))
        calls = []
        def fake_command(command, **kwargs):
            calls.append(command)
            if "--output" in command:
                target = Path(command[command.index("--output") + 1]) / candidate.name
                shutil.copyfile(candidate, target)
            return SimpleNamespace(returncode=0, stderr="", stdout="")
        with patch.object(installer.shutil, "which", return_value="/mock/tool"), \
             patch.object(installer.subprocess, "run", side_effect=fake_command), \
             contextlib.redirect_stdout(io.StringIO()):
            installer.main(argv)
        self.assertEqual([cmd[0] for cmd in calls[-4:]],
                         ["cmake", "cmake", sys.executable, sys.executable])
        self.assertIn("-DFRAMEYAP_NATIVE=ON", calls[-4])
        self.assertIn("-DFRAMEYAP_VERSION=0.1.202609241531", calls[-4])
        self.assertIn(["sh", str(sources / "scripts/install-preflight.sh"), "--source"], calls)
        self.assertEqual(calls[-1][calls[-1].index("--model-revision") + 1], "a" * 40)
        self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/0.1.202609241531")
        self.assertEqual(os.readlink(self.data / "frameyap/previous"), "versions/0.1.202609241530")

    def test_expected_installed_manifest_hash_is_id_bound_and_read_only_on_mismatch(self):
        shutil.copyfile(REPO / "python/frameyap/model_files.py",
                        self.stage / "python/frameyap/model_files.py")
        manifest = json.loads((REPO / "assets/backends/redux.json").read_text())
        payload = b"pinned local test bytes"
        manifest["model"]["files"] = [{"path": "weights.bin", "size": len(payload),
                                       "sha256": hashlib.sha256(payload).hexdigest()}]
        manifests = self.stage / "assets/backends"
        manifests.mkdir()
        # Deliberate formatting: the contract is raw bytes, not canonical JSON.
        raw = (json.dumps(manifest, indent=3) + "\n").encode()
        (manifests / "redux.json").write_bytes(raw)
        other = {**manifest, "id": "other", "display_name": "Other fixture backend"}
        (manifests / "other.json").write_text(json.dumps(other))
        archive, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", archive, digest, "--without-model")
        root = self.data / "frameyap"
        installed_raw = (root / "current/assets/backends/redux.json").read_bytes()
        self.assertEqual(installed_raw, raw)
        expected = hashlib.sha256(installed_raw).hexdigest()
        other_sha = hashlib.sha256((root / "current/assets/backends/other.json").read_bytes()).hexdigest()
        self.assertNotEqual(other_sha, expected)
        destination = self.base / "never-created"
        argv = ["--install-model", "--backend", "redux", "--model-dir", str(destination),
                "--expected-manifest-sha256", "0" * 64, "--json"]
        with patch.object(installer.urllib.request, "urlopen") as fetch:
            for extra in (["--yes"], ["--print-plan"]):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(installer.cli(argv + extra), 1)
                error = json.loads(output.getvalue())
                self.assertEqual((error["code"], error["exit_code"]), ("manifest_mismatch", 1))
                self.assertIn("redux.json", error["message"])
            fetch.assert_not_called()
        self.assertFalse(destination.exists())
        self.assertFalse((root / "models").exists())
        with (root / ".model.lock").open("a+b") as provision_lock:
            fcntl.flock(provision_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, r"\.model\.lock held"):
                installer.main(argv + ["--yes"])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(installer.cli(argv[:-3] + ["--expected-manifest-sha256", "bad", "--yes", "--json"]), 2)
        self.assertEqual(json.loads(output.getvalue())["code"], "usage")
        self.assertFalse(destination.exists())
        correct = argv[:-3] + ["--expected-manifest-sha256", expected.upper(), "--print-plan", "--json"]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(installer.cli(correct), 0)
        plan = json.loads(output.getvalue())
        self.assertEqual(plan["installed_manifest_sha256"], expected)
        self.assertEqual(plan["expected_manifest_sha256"], expected)
        self.assertFalse(destination.exists())
        destination.mkdir()
        (destination / "weights.bin").write_bytes(payload)
        with patch.object(installer.urllib.request, "urlopen") as fetch:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(installer.cli(correct[:-2] + ["--yes", "--json"]), 0)
            fetch.assert_not_called()
        self.assertEqual([json.loads(line)["event"] for line in output.getvalue().splitlines()],
                         ["model_file", "complete"])
        # The digest of a different valid manifest cannot authorize this ID.
        with patch.object(installer.urllib.request, "urlopen") as fetch:
            for selected_id, wrong_sha in (("redux", other_sha), ("other", expected)):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(installer.cli(["--install-model", "--backend", selected_id,
                                                   "--expected-manifest-sha256", wrong_sha, "--yes", "--json"]), 1)
                self.assertEqual(json.loads(output.getvalue())["code"], "manifest_mismatch")
            fetch.assert_not_called()
        self.assertFalse((root / "models/other").exists())

    def test_model_install_manifest_verification_and_explicit_consent(self):
        # Tiny pinned files via the shared verifier; urllib is mocked, no network.
        source_module = REPO / "python/frameyap/model_files.py"
        staged_module = self.stage / "python/frameyap/model_files.py"
        shutil.copyfile(source_module, staged_module)
        manifest = json.loads((REPO / "assets/backends/redux.json").read_text())
        payload = b"tiny pinned model fixture"
        manifest["model"]["files"] = [{"path": "sub/weights.bin", "size": len(payload),
                                           "sha256": hashlib.sha256(payload).hexdigest()}]
        manifests = self.stage / "assets/backends"
        manifests.mkdir()
        (manifests / "redux.json").write_text(json.dumps(manifest))
        archive, digest = self.package("0.1.202609241530")
        self.install("0.1.202609241530", archive, digest, "--without-model")
        destination = self.base / "models"
        argv = ["--install-model", "--backend", "redux", "--model-dir", str(destination), "--json"]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(installer.cli(argv), 1)
        self.assertIn("--yes", json.loads(output.getvalue())["message"])
        self.assertFalse(destination.exists())
        with (self.data / "frameyap/.model.lock").open("a+b") as provision_lock:
            fcntl.flock(provision_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "model provisioning is running"):
                installer.main(["--rollback"])
        class Response(io.BytesIO):
            def geturl(self):
                return "https://huggingface.co/fixture"
        with patch.object(installer.urllib.request, "urlopen", return_value=Response(b"x" * len(payload))):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(installer.cli(argv + ["--yes"]), 1)
            self.assertIn("SHA-256/size mismatch", output.getvalue())
            self.assertFalse((destination / "sub/weights.bin").exists())
        with patch.object(installer.urllib.request, "urlopen", return_value=Response(payload)) as fetch, \
             (self.data / "frameyap/.lock").open("a+b") as app_lock:
            fcntl.flock(app_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)  # UI's running app
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(installer.cli(argv + ["--yes"]), 0)
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual([event["event"] for event in events],
                             ["model_file", "model_file", "complete"])
            self.assertEqual((destination / "sub/weights.bin").read_bytes(), payload)
            self.assertEqual(fetch.call_count, 1)
        with patch.object(installer.urllib.request, "urlopen") as fetch:
            repeated = io.StringIO()
            with contextlib.redirect_stdout(repeated):
                repeated_code = installer.cli(argv + ["--yes"])
            self.assertEqual(repeated_code, 0, repeated.getvalue())
            fetch.assert_not_called()
        (destination / "sub/weights.bin").write_bytes(b"bad")
        with patch.object(installer.urllib.request, "urlopen") as fetch:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(installer.cli(argv + ["--yes"]), 1)
            self.assertIn("mismatched", json.loads(output.getvalue())["message"])
            fetch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
