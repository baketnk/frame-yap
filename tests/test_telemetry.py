"""Offline analytics choice tests; optional exact-wheel reporter audit uses fake HTTP.

Set FRAMEYAP_TEST_KESTREL_WHEEL to a previously downloaded Kestrel 0.8.0 wheel
for the upstream lifecycle test. Tests never download, infer or upload telemetry.
"""
import asyncio
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
from frameyap import worker

VERSIONS = {"kestrel": "0.8.0", "moondream": "2.4.0"}
WHEEL_SHA256 = "15c65bf12983d347330082838d4c6a9678a7f00c0f32ae089d9ccad7482e62bd"


class TelemetryTests(unittest.TestCase):
    def reporter_modules(self, reporter):
        return {"kestrel": types.ModuleType("kestrel"),
                "kestrel.photon": types.SimpleNamespace(PhotonReporter=reporter)}

    def test_flush_disabled_including_retry_and_repeated_setup(self):
        class Reporter:
            async def _flush_window(self, *, rotate=True):
                raise AssertionError("upload attempted")
        with patch.dict(sys.modules, self.reporter_modules(Reporter)), \
                patch.object(worker.importlib.metadata, "version", side_effect=VERSIONS.__getitem__):
            for _ in range(2):
                worker.disable_kestrel_telemetry()
                self.assertIsNone(asyncio.run(Reporter()._flush_window()))
                self.assertIsNone(asyncio.run(Reporter()._flush_window(rotate=False)))

    def test_unknown_versions_fail_before_importing_kestrel(self):
        for package in VERSIONS:
            versions = {**VERSIONS, package: "999.0"}
            with self.subTest(package=package), \
                    patch.object(worker.importlib.metadata, "version", side_effect=versions.__getitem__), \
                    patch.dict(sys.modules, {"kestrel": None, "kestrel.photon": None}):
                with self.assertRaisesRegex(worker.TelemetryProtectionError, "unsupported runtime version"):
                    worker.disable_kestrel_telemetry()

    def test_unknown_interfaces_fail_closed(self):
        async def positional(self, rotate=True): pass
        async def wrong_default(self, *, rotate=False): pass
        for flush in (None, lambda self: None, positional, wrong_default):
            reporter = type("Reporter", (), {"_flush_window": flush})
            with self.subTest(flush=flush), patch.dict(sys.modules, self.reporter_modules(reporter)), \
                    patch.object(worker.importlib.metadata, "version", side_effect=VERSIONS.__getitem__):
                with self.assertRaisesRegex(worker.TelemetryProtectionError, "unsupported telemetry reporter interface"):
                    worker.disable_kestrel_telemetry()

    def test_choice_applied_before_model_creation_default_and_malformed_off(self):
        torch = types.SimpleNamespace(set_num_threads=Mock(), set_num_interop_threads=Mock())
        for value in (None, "0", "", "true", "garbage", "1"):
            calls = []
            md = types.SimpleNamespace(photon=lambda *a, **kw: calls.append("model"))
            with self.subTest(value=value), patch.dict(os.environ, clear=True), \
                    patch.dict(sys.modules, {"torch": torch, "moondream": md}), \
                    patch.object(worker, "local_model", return_value="/fixture"), \
                    patch.object(worker, "disable_kestrel_telemetry", side_effect=lambda: calls.append("guard")):
                if value is not None:
                    os.environ["FRAMEYAP_MOONDREAM_ANALYTICS"] = value
                worker.load_model("/fixture", 2)
                self.assertEqual(calls, ["model"] if value == "1" else ["guard", "model"])

    def test_failed_guard_prevents_engine_start(self):
        torch = types.SimpleNamespace(set_num_threads=Mock(), set_num_interop_threads=Mock())
        photon = Mock()
        with patch.dict(os.environ, {"FRAMEYAP_MOONDREAM_ANALYTICS": "0"}), \
                patch.dict(sys.modules, {"torch": torch, "moondream": types.SimpleNamespace(photon=photon)}), \
                patch.object(worker, "local_model", return_value="/fixture"), \
                patch.object(worker, "disable_kestrel_telemetry", side_effect=worker.TelemetryProtectionError("blocked")):
            with self.assertRaises(worker.TelemetryProtectionError):
                worker.load_model("/fixture", 2)
            photon.assert_not_called()

    def test_privacy_failure_has_specific_protocol_code_without_private_details(self):
        program = '''from frameyap import worker

def blocked(*args):
    raise worker.TelemetryProtectionError("PRIVATE DETAILS")
worker.load_model = blocked
raise SystemExit(worker.main())
'''
        with tempfile.TemporaryDirectory() as clip:
            os.chmod(clip, 0o700)
            result = subprocess.run([sys.executable, "-c", program, "--model", "unused", "--clip-dir", clip],
                capture_output=True, timeout=5,
                env={**os.environ, "PYTHONPATH": str(ROOT / "python"), "FRAMEYAP_ADVANCED_DEBUG": "0"})
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, struct.pack("<I", 2) + b"FP")
            self.assertEqual(result.stderr, b"")


@unittest.skipUnless(os.environ.get("FRAMEYAP_TEST_KESTREL_WHEEL"), "optional audited upstream wheel not supplied")
class PinnedReporterTests(unittest.TestCase):
    def test_on_and_off_startup_periodic_shutdown_and_auth_retry(self):
        wheel = Path(os.environ["FRAMEYAP_TEST_KESTREL_WHEEL"])
        self.assertEqual(hashlib.sha256(wheel.read_bytes()).hexdigest(), WHEEL_SHA256)
        with zipfile.ZipFile(wheel) as archive:
            source = archive.read("kestrel/photon.py")
        class Client:
            def __init__(self, *args, **kwargs):
                self.headers = kwargs.get("headers") or {}
                self.posts = []
                self.closed = False
            async def post(self, url, **kwargs):
                self.posts.append((url, kwargs))
                # Auth failure drives upstream's actual nonrotating retry path.
                code = 401 if len(self.posts) == 1 and self.headers else 200
                return types.SimpleNamespace(status_code=code, json=lambda: {"standing": "anonymous"})
            async def aclose(self):
                self.closed = True

        for enabled in (False, True, False):  # each setting reload uses a fresh process/class
            for key in (None, "non-secret-fixture-key", "invalid fixture key"):
                module = types.ModuleType("kestrel.photon")
                modules = {"kestrel": types.ModuleType("kestrel"), "kestrel.photon": module,
                    "kestrel.config": types.SimpleNamespace(RuntimeConfig=object),
                    "httpx": types.SimpleNamespace(AsyncClient=Client, HTTPError=OSError),
                    "torch": types.SimpleNamespace()}
                with self.subTest(enabled=enabled, key=key), patch.dict(sys.modules, modules), \
                        patch.object(worker.importlib.metadata, "version", side_effect=VERSIONS.__getitem__), \
                        patch("socket.gethostname", return_value="fixture-host"):
                    exec(compile(source, "audited-kestrel-0.8.0/photon.py", "exec"), module.__dict__)
                    module._TELEMETRY_FLUSH_INTERVAL_SECONDS = 0.001
                    if not enabled:
                        worker.disable_kestrel_telemetry()

                    async def exercise():
                        reporter = module.PhotonReporter(
                            types.SimpleNamespace(service_name="local", model="moondream/parakeet-redux"),
                            types.SimpleNamespace(type="cpu"), api_key=key,
                            api_base_url="https://api.moondream.ai")
                        client = reporter._client
                        await reporter.validate_api_key()
                        startup = len(client.posts)
                        reporter.record_success(finetune=None, input_tokens=3, output_tokens=4)
                        reporter.record_error(finetune=None)
                        flushed = asyncio.Event()
                        original = reporter._flush_window
                        async def observed(*, rotate=True):
                            result = await original(rotate=rotate)
                            flushed.set()
                            return result
                        reporter._flush_window = observed
                        reporter.start()
                        await asyncio.wait_for(flushed.wait(), timeout=1)
                        periodic = len(client.posts)
                        await reporter.shutdown()
                        shutdown = len(client.posts)
                        await reporter._flush_window(rotate=False)
                        self.assertTrue(client.closed)
                        if not enabled:
                            self.assertEqual(client.posts, [])
                        else:
                            self.assertGreater(startup, 0)
                            self.assertGreater(periodic, startup)
                            self.assertGreater(shutdown, periodic)
                            report = client.posts[0][1]["json"]["reports"][0]
                            self.assertEqual(client.posts[0][0], "https://api.moondream.ai/v1/photon/telemetry")
                            self.assertEqual(report["hostname"], "fixture-host")
                            self.assertNotIn("audio", report)
                            self.assertNotIn("text", report)
                    asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
