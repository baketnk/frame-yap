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
        a1, h1 = self.package("v1")
        self.install("v1", a1, h1)
        root = self.data / "frameyap"
        self.assertEqual(os.readlink(root / "current"), "versions/v1")
        launcher = self.home / ".local/bin/frameyap"
        self.assertIn("--run", launcher.read_text())
        self.assertNotIn("--font", launcher.read_text())  # run/check modes honor config font
        config = self.home / ".config/frameyap/config.json"
        self.assertEqual(json.loads(config.read_text()), installer.CONFIG_DEFAULTS)
        self.assertIs(json.loads(config.read_text())["advanced_debug"], False)
        self.assertIs(json.loads(config.read_text())["auto_insert"], False)
        self.assertIs(json.loads(config.read_text())["lock_layout"], False)
        self.assertEqual(list(config.parent.glob("config.json.backup-*")), [])
        self.assertIn("PYTHONDONTWRITEBYTECODE=1", launcher.read_text())
        self.assertTrue(os.access(root / "versions/v1/runtime/bin/helper", os.X_OK))
        self.assertTrue(os.access(root / "versions/v1/lib/libtest.so", os.X_OK))
        manifest = json.loads((root / "frameyap.vrmanifest").read_text())
        self.assertEqual(manifest["applications"][0]["app_key"], "local.frameyap.overlay")
        self.assertEqual(manifest["applications"][0]["binary_path_linux"], str(launcher))
        self.assertEqual(manifest["applications"][0]["binary_path_linux_arm"], str(launcher))
        self.assertNotIn("binary_path", manifest["applications"][0])
        desktop = self.data / "applications/frameyap.desktop"
        self.assertIn(f'Exec="{launcher}"', desktop.read_text())
        self.assertIn("Terminal=false", desktop.read_text())
        (root / "config-untouched").write_text("keep")
        self.install("v1", a1, h1)
        (self.stage / "bin/frameyap").write_text("next binary")
        a2, h2 = self.package("v2")
        self.install("v2", a2, h2)
        self.assertEqual(os.readlink(root / "current"), "versions/v2")
        self.assertEqual(os.readlink(root / "previous"), "versions/v1")
        (root / "frameyap.vrmanifest").write_text("foreign")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            installer.main(["--rollback"])
        self.assertEqual(os.readlink(root / "current"), "versions/v2")
        (root / "frameyap.vrmanifest").unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--rollback"])
        self.assertEqual(os.readlink(root / "current"), "versions/v1")
        self.assertEqual(os.readlink(root / "previous"), "versions/v2")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            installer.main(["--uninstall"])
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--uninstall", "--unregistered"])
        self.assertEqual((root / "config-untouched").read_text(), "keep")
        self.assertTrue((root / "saved-models/v1/weights.bin").exists())
        self.assertTrue((root / "saved-models/v2/weights.bin").exists())
        self.assertFalse(launcher.exists())
        self.assertFalse(desktop.exists())

    def test_config_install_repairs_and_preserves_original(self):
        archive, digest = self.package("v1")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        original = b'{"font":"/system/face.ttf","theme":{"ink":"#F1f2F3"},"buttons":{"ptt":"/user/hand/left/input/y"}}\n'
        config.write_bytes(original)
        self.install("v1", archive, digest)
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
        fixed["lock_layout"] = True
        fixed["clock_24h"] = True
        fixed["date_format"] = "iso"
        fixed["buttons"]["enter"] = ""  # intentional disabling survives upgrades
        fixed["quick_inputs"] = ["/new", "/questions", "hello there"]
        fixed["wrist"]["y"] = 0.2
        compact = json.dumps(fixed, separators=(",", ":")).encode()
        config.write_bytes(compact)
        self.install("v1", archive, digest)
        self.assertEqual(config.read_bytes(), compact)
        self.assertIs(json.loads(config.read_text())["advanced_debug"], True)
        self.assertIs(json.loads(config.read_text())["auto_insert"], True)
        self.assertIs(json.loads(config.read_text())["lock_layout"], True)
        self.assertIs(json.loads(config.read_text())["clock_24h"], True)
        self.assertEqual(json.loads(config.read_text())["date_format"], "iso")
        self.assertEqual(len(list(config.parent.glob("config.json.backup-*"))), 1)
        fixed["advanced_debug"] = False
        compact_off = json.dumps(fixed, separators=(",", ":")).encode()
        config.write_bytes(compact_off)
        self.install("v1", archive, digest)
        self.assertEqual(config.read_bytes(), compact_off)
        self.assertEqual(len(list(config.parent.glob("config.json.backup-*"))), 1)
        original = b'{"font":"/system/face.ttf","input_priority":"highest","wrist":{"x":0.04,"y":true,"width":100,"obsolete":4},"theme":{"ink":"bad","retired":"#123456"},"buttons":{"ptt":"/user/hand/left/input/grip"},"old_option":4}'
        config.write_bytes(original)
        self.install("v1", archive, digest)
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
        self.install("v1", archive, digest)
        self.assertEqual(json.loads(config.read_text()), installer.CONFIG_DEFAULTS)
        self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])
        config.write_text(json.dumps({"font": "/" + "x" * 3800}))
        self.install("v1", archive, digest)
        self.assertEqual(json.loads(config.read_text())["font"], "")
        self.assertTrue(all(len(p.read_bytes()) > 0 for p in config.parent.glob("config.json.backup-*")))
        config.write_bytes(b"x" * 65537)
        with self.assertRaisesRegex(ValueError, "too large"):
            self.install("v1", archive, digest)
        self.assertEqual(config.stat().st_size, 65537)
        config.unlink()
        config.symlink_to(self.stage / "model/weights.bin")
        with self.assertRaisesRegex(ValueError, "foreign config path"):
            self.install("v1", archive, digest)
        self.assertTrue(config.is_symlink())

    def test_debug_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("v1")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"advanced_debug": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("v1", archive, digest)
            self.assertIs(json.loads(config.read_text())["advanced_debug"], False)
            self.assertEqual(json.loads(config.read_text())["font"], "/custom/font.ttf")
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_auto_insert_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("v1")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"auto_insert": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("v1", archive, digest)
            self.assertIs(json.loads(config.read_text())["auto_insert"], False)
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_lock_layout_boolean_repair_backs_up_invalid_values(self):
        archive, digest = self.package("v1")
        config = self.home / ".config/frameyap/config.json"
        config.parent.mkdir(parents=True)
        for invalid in ("true", 1, None, [], {}):
            original = json.dumps({"lock_layout": invalid, "font": "/custom/font.ttf"}).encode()
            config.write_bytes(original)
            self.install("v1", archive, digest)
            fixed = json.loads(config.read_text())
            self.assertIs(fixed["lock_layout"], False)
            self.assertEqual(fixed["font"], "/custom/font.ttf")
            self.assertIn(original, [p.read_bytes() for p in config.parent.glob("config.json.backup-*")])

    def test_digest_and_same_version_mismatch_leave_previous(self):
        a, h = self.package("v1")
        self.install("v1", a, h)
        root = self.data / "frameyap"
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.install("v1", a, "0" * 64)
        a.unlink()
        (self.output / (a.name + ".sha256")).unlink()
        (self.stage / "bin/frameyap").write_text("changed")
        a, h2 = self.package("v1")
        with self.assertRaisesRegex(ValueError, "different archive digest"):
            self.install("v1", a, h2)
        self.assertEqual(os.readlink(root / "current"), "versions/v1")

    def test_without_model_lock_and_foreign_launcher(self):
        a, h = self.package("v1")
        lock = self.data / "frameyap/.lock"
        lock.parent.mkdir(parents=True)
        with lock.open("a+b") as fd:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "running"):
                self.install("v1", a, h)
        self.install("v1", a, h, "--without-model")
        root = self.data / "frameyap"
        self.assertFalse((root / "versions/v1/model").exists())
        self.assertTrue((self.stage / "model/weights.bin").exists())
        self.assertEqual(json.loads((root / "versions/v1/.install-options.json").read_text()),
                         {"without_model": True})
        self.install("v1", a, h, "--without-model")
        self.assertFalse((root / "versions/v1/model").exists())
        with self.assertRaisesRegex(ValueError, "different --without-model choice"):
            self.install("v1", a, h)
        self.assertFalse((root / "versions/v1/model").exists())
        (root / "frameyap.vrmanifest").unlink()
        launcher = self.home / ".local/bin/frameyap"
        launcher.unlink()
        self.install("v1", a, h, "--without-model")
        self.assertTrue(launcher.exists())
        self.assertTrue((root / "frameyap.vrmanifest").exists())
        (root / "frameyap.vrmanifest").write_text("foreign")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("v1", a, h, "--without-model")
        (root / "frameyap.vrmanifest").unlink()
        self.install("v1", a, h, "--without-model")
        (self.home / ".local/bin/frameyap").write_text("#!/bin/sh\n" + installer.MARKER + "echo foreign\n")
        (self.stage / "bin/frameyap").write_text("new")
        a2, h2 = self.package("v2")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("v2", a2, h2)
        self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/v1")
        self.assertFalse((self.data / "frameyap/versions/v2").exists())

    def test_foreign_desktop_entry_is_preserved(self):
        a, h = self.package("v1")
        desktop = self.data / "applications/frameyap.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text("foreign shortcut\n")
        with self.assertRaisesRegex(ValueError, "foreign file"):
            self.install("v1", a, h)
        self.assertEqual(desktop.read_text(), "foreign shortcut\n")
        self.assertFalse((self.data / "frameyap/current").exists())

    def test_desktop_exec_escapes_field_codes(self):
        entry = installer.desired_desktop(Path('/home/steam%user/.local/bin/frameyap')).decode()
        self.assertIn('Exec="/home/steam%%user/.local/bin/frameyap"', entry)

    def test_traversal_and_link_archives_rejected(self):
        a, h = self.package("v1")
        self.install("v1", a, h)
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
                    self.install("v2", bad, sha)
                self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/v1")
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
                    self.install("v1", bad, sha)

    def test_uninstall_refuses_untracked_files(self):
        a, h = self.package("v1")
        self.install("v1", a, h)
        root = self.data / "frameyap"
        extra = root / "versions/v1/user.txt"
        extra.write_text("preserve")
        with self.assertRaisesRegex(ValueError, "untracked files"):
            with contextlib.redirect_stdout(io.StringIO()):
                installer.main(["--uninstall", "--unregistered"])
        self.assertTrue(extra.exists())
        self.assertEqual(os.readlink(root / "current"), "versions/v1")

    def test_release_metadata_mismatch_retains_current(self):
        a, h = self.package("v1")
        self.install("v1", a, h)
        a2, h2 = self.package("v2")
        with self.assertRaisesRegex(ValueError, "metadata version/architecture/schema mismatch"):
            self.install("v3", a2, h2)
        self.assertEqual(os.readlink(self.data / "frameyap/current"), "versions/v1")

    def test_launcher_defaults_run_but_forwards_registration(self):
        path = self.stage / "bin/frameyap"
        path.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\nprintf "ENV:%s\\n" "${PYTHONDONTWRITEBYTECODE:-}"\n')
        path.chmod(0o755)
        a, h = self.package("v1")
        self.install("v1", a, h)
        launcher = self.home / ".local/bin/frameyap"
        reg = subprocess.run([str(launcher), "--register", "test-manifest"], capture_output=True, text=True, check=True)
        self.assertEqual(reg.stdout.splitlines(), ["--register", "test-manifest", "ENV:1"])
        run = subprocess.run([str(launcher)], capture_output=True, text=True, check=True,
                             env={**os.environ, "GAMESCOPE_SOCKET": "fixture-socket"})
        self.assertEqual(run.stdout.splitlines()[0], "--run")
        self.assertIn("--assets", run.stdout)
        self.assertIn("--socket\nfixture-socket\n", run.stdout)
        self.assertIn("ENV:1", run.stdout)
        self.assertEqual(json.loads((self.data / "frameyap/versions/v1/.install-options.json").read_text()),
                         {"without_model": False})
        with self.assertRaisesRegex(ValueError, "different --without-model choice"):
            self.install("v1", a, h, "--without-model")
        self.assertTrue((self.data / "frameyap/versions/v1/model/weights.bin").exists())

    def test_no_model_reinstall_preserves_provisioned_model_and_uninstall(self):
        a, h = self.package("v1")
        self.install("v1", a, h, "--without-model")
        root = self.data / "frameyap"
        model = root / "versions/v1/model"
        model.mkdir()
        (model / "provided.bin").write_text("user-provided")
        self.install("v1", a, h, "--without-model")
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main(["--uninstall", "--unregistered"])
        self.assertEqual((root / "saved-models/v1/provided.bin").read_text(), "user-provided")

    def test_version_switch_rejects_untracked_installed_files(self):
        a, h = self.package("v1")
        self.install("v1", a, h)
        (self.data / "frameyap/versions/v1/untracked").write_text("keep")
        a2, h2 = self.package("v2")
        with self.assertRaisesRegex(ValueError, "untracked files"):
            self.install("v1", a, h)
        # Upgrade does not delete the old directory, but uninstall must still refuse it.
        self.install("v2", a2, h2)
        with self.assertRaisesRegex(ValueError, "untracked files"):
            installer.main(["--uninstall", "--unregistered"])

    def test_external_authorized_runtime_is_explicit(self):
        import shutil
        shutil.rmtree(self.stage / "runtime")
        native = self.stage / "bin/frameyap"
        native.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        native.chmod(0o755)
        a, h = self.package("external", "--external-runtime")
        self.install("external", a, h)
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
        a, h = self.package("external", "--external-runtime")
        self.install("external", a, h)
        root = self.data / "frameyap"
        launcher = self.home / ".local/bin/frameyap"
        # A pre-config release launcher may be upgraded only when its exact
        # bytes match the managed legacy template, not just its marker.
        launcher.write_bytes(installer.desired_launcher(root, legacy=True))
        self.install("external", a, h)
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
            self.install("external", a, h)

    def test_package_rejects_symlink(self):
        (self.stage / "lib/link.so").symlink_to("libtest.so")
        result = subprocess.run([sys.executable, str(REPO / "scripts/package-release.py"), "--stage", str(self.stage),
            "--output", str(self.output), "--arch", "linux-aarch64", "--version", "v1",
            "--model-revision", "test"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("links and special files forbidden", result.stderr)

    def test_utc_timestamp_release_tag_roundtrip(self):
        version = "2026-09-24T162712Z-g417f81c-dirty"
        archive, digest = self.package(version)
        self.assertEqual(archive.name, f"frameyap-{version}-linux-aarch64.tar.gz")
        self.install(version, archive, digest)
        root = self.data / "frameyap"
        self.assertEqual(os.readlink(root / "current"), f"versions/{version}")
        self.assertEqual(json.loads((root / "current/release.json").read_text())["version"], version)


if __name__ == "__main__":
    unittest.main()
