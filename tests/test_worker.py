"""Hardware-free tests, and a deliberately fake child for native IPC tests.

Run: python3 -m unittest discover -s tests -p test_worker.py
Native fixture: python3 tests/test_worker.py --model ok --threads 2 --clip-dir DIR
"""
import os
import io
import contextlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from frameyap import worker


class FakeModel:
    def __init__(self):
        self.calls = []

    def transcribe(self, audio, sample_rate):
        self.calls.append((audio, sample_rate))
        return {"text": "héllo 世界"}


def fake_child():
    # This branch intentionally never imports numpy, torch, moondream or a mic.
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--threads", required=True)
    parser.add_argument("--clip-dir", required=True)
    parser.add_argument("--advanced-debug", action="store_true")
    args = parser.parse_args()
    if args.model in ("fail", "missing-model", "missing-import"):
        worker.send_frame(1, b"F", {"fail": b"", "missing-model": b"M",
                                      "missing-import": b"I"}[args.model])
        return
    if args.model == "crash":
        return
    if args.model == "hang-warm":
        import time
        time.sleep(130)
        return
    worker.send_frame(1, b"Y")
    requests = 0
    while True:
        msg = worker.read_frame(0)
        if msg is None:
            return
        if args.model == "hang":
            import time
            time.sleep(70)
            continue
        assert len(msg) == 9 and msg[:1] == b"T"
        requests += 1
        if args.advanced_debug and args.model in ("debug-output", "debug-spam"):
            os.write(2, b"private debug fixture\n")
            if args.model == "debug-spam":
                for _ in range(1100):
                    os.write(2, b"x" * 4096)
        clip = Path(args.clip_dir) / "clip.raw"
        assert clip.stat().st_size == 3200 * 4
        ident = msg[1:9]
        if args.model == "stale":
            ident = struct.pack("<Q", struct.unpack("<Q", ident)[0] + 1)
        if args.model == "oversized-frame":
            os.write(1, struct.pack("<I", 65537))
            continue
        errors = {"request-error": b"transcription failed",
                  "safe-error": b"transcription failed [inference: RuntimeError]",
                  "unsafe-error": b"transcription failed [inference: RuntimeError] PRIVATE SPEECH\n"}
        if args.model in errors and requests == 1:
            worker.send_frame(1, b"E", ident + errors[args.model])
            continue
        text = b"a" * 4097 if args.model == "long" else "héllo 世界".encode()
        worker.send_frame(1, b"R", ident + text)
        if args.model == "duplicate":
            worker.send_frame(1, b"R", ident + text)


class WorkerTests(unittest.TestCase):
    def test_framing_and_bounds(self):
        r, w = os.pipe()
        try:
            worker.send_frame(w, b"T", struct.pack("<Q", 23))
            self.assertEqual(worker.read_frame(r), b"T" + struct.pack("<Q", 23))
            os.write(w, struct.pack("<I", 65537))
            with self.assertRaises(ValueError):
                worker.read_frame(r)
        finally:
            os.close(r)
            os.close(w)

    def test_fake_transcription_correlated_and_private(self):
        with tempfile.TemporaryDirectory() as path:
            os.chmod(path, 0o700)
            clip = Path(path) / "clip.raw"
            clip.write_bytes(struct.pack("<3200f", *([0.0] * 3200)))
            os.chmod(clip, 0o600)
            model = FakeModel()
            with patch.object(worker, "read_clip", return_value=[0.0] * 3200):
                in_r, in_w = os.pipe()
                out_r, out_w = os.pipe()
                try:
                    worker.send_frame(in_w, b"T", struct.pack("<Q", 42))
                    os.close(in_w)
                    in_w = -1
                    worker.run(model, path, in_r, out_w)
                    self.assertEqual(worker.read_frame(out_r), b"Y")
                    reply = worker.read_frame(out_r)
                    self.assertEqual(reply, b"R" + struct.pack("<Q", 42) + "héllo 世界".encode())
                    self.assertEqual(model.calls[0][1], 16000)
                finally:
                    os.close(in_r)
                    if in_w >= 0:
                        os.close(in_w)
                    os.close(out_r)
                    os.close(out_w)

    def test_safe_error_labels_and_retry(self):
        class PrivateError(Exception):
            def __str__(self):
                raise AssertionError("exception messages must not be accessed")
        self.assertEqual(worker.safe_request_error("PRIVATE PATH", PrivateError()),
                         b"transcription failed [unknown: Exception]")
        cases = [
            (OSError("PRIVATE AUDIO"), {"text": "ignored"}, b"audio: OSError"),
            (None, RuntimeError("PRIVATE TRANSCRIPT"), b"inference: RuntimeError"),
            (None, PrivateError(), b"inference: Exception"),
            (None, {}, b"response: KeyError"),
            (None, {"text": 4}, b"response: TypeError"),
            (None, {"text": "\ud800"}, b"response: UnicodeError"),
        ]
        for audio_error, response, label in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as path:
                class Model:
                    count = 0
                    def transcribe(self, **kwargs):
                        self.count += 1
                        if self.count > 1:
                            return {"text": "retry"}
                        if isinstance(response, Exception):
                            raise response
                        return response
                model = Model()
                in_r, in_w = os.pipe()
                out_r, out_w = os.pipe()
                try:
                    worker.send_frame(in_w, b"T", struct.pack("<Q", 1))
                    worker.send_frame(in_w, b"T", struct.pack("<Q", 2))
                    os.close(in_w); in_w = -1
                    # A clip-read failure never calls the model; retry can succeed.
                    reads = [audio_error, []] if audio_error else [[], []]
                    if audio_error:
                        model.count = 1
                    with patch.object(worker, "read_clip", side_effect=reads):
                        worker.run(model, path, in_r, out_w)
                    self.assertEqual(worker.read_frame(out_r), b"Y")
                    self.assertEqual(worker.read_frame(out_r), b"E" + struct.pack("<Q", 1) +
                                     b"transcription failed [" + label + b"]")
                    self.assertEqual(worker.read_frame(out_r), b"R" + struct.pack("<Q", 2) + b"retry")
                finally:
                    os.close(in_r)
                    if in_w >= 0: os.close(in_w)
                    os.close(out_r); os.close(out_w)

    def test_debug_is_explicit_and_protocol_stays_safe(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as path:
                class Failing:
                    def transcribe(self, **kwargs):
                        raise RuntimeError("PRIVATE EXCEPTION DETAIL")
                in_r, in_w = os.pipe(); out_r, out_w = os.pipe()
                captured = io.StringIO()
                try:
                    worker.send_frame(in_w, b"T", struct.pack("<Q", 8))
                    os.close(in_w); in_w = -1
                    with patch.object(worker, "read_clip", return_value=[]), contextlib.redirect_stderr(captured):
                        worker.run(Failing(), path, in_r, out_w, advanced_debug=enabled)
                    self.assertEqual(worker.read_frame(out_r), b"Y")
                    self.assertEqual(worker.read_frame(out_r), b"E" + struct.pack("<Q", 8) +
                                     b"transcription failed [inference: RuntimeError]")
                    if enabled:
                        self.assertIn("Traceback", captured.getvalue())
                        self.assertIn("PRIVATE EXCEPTION DETAIL", captured.getvalue())
                    else:
                        self.assertEqual(captured.getvalue(), "")
                finally:
                    os.close(in_r)
                    if in_w >= 0: os.close(in_w)
                    os.close(out_r); os.close(out_w)

    def test_debug_fd_output_never_corrupts_protocol(self):
        program = '''
import os, sys
from frameyap import worker
class Model:
    def transcribe(self, **kwargs):
        os.write(1, b"native stdout fixture\\n")
        os.write(2, b"native stderr fixture\\n")
        return {"text": "private fixture transcript"}
worker.load_model = lambda *args: Model()
worker.read_clip = lambda *args: []
sys.exit(worker.main(sys.argv[1:]))
'''
        request = b"T" + struct.pack("<Q", 12)
        response = b"R" + struct.pack("<Q", 12) + b"private fixture transcript"
        expected = struct.pack("<I", 1) + b"Y" + struct.pack("<I", len(response)) + response
        with tempfile.TemporaryDirectory() as path:
            for enabled in (False, True):
                args = [sys.executable, "-c", program, "--model", "unused", "--clip-dir", path]
                if enabled: args.append("--advanced-debug")
                env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[1] / "python"))
                result = subprocess.run(args, input=struct.pack("<I", len(request)) + request,
                                        capture_output=True, env=env, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)
                if enabled:
                    self.assertIn(b"native stdout fixture", result.stderr)
                    self.assertIn(b"native stderr fixture", result.stderr)
                    self.assertIn(b"private fixture transcript", result.stderr)
                else:
                    self.assertEqual(result.stderr, b"")

    def test_missing_weights_no_dependency_import(self):
        with tempfile.TemporaryDirectory() as path:
            with self.assertRaisesRegex(worker.LocalModelError, "weights missing"):
                worker.local_model(path)
            with patch.dict(sys.modules, {"moondream": None}):
                with self.assertRaises(ValueError):
                    worker.load_model(path, 2)
            self.assertEqual(os.environ["HF_HUB_OFFLINE"], "1")

    def test_pinned_model_hashes_and_symlinks(self):
        import hashlib
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            (root / "model.safetensors").write_bytes(b"fixture")
            digest = hashlib.sha256(b"fixture").hexdigest()
            with patch.object(worker, "FILES", {"model.safetensors": (7, digest)}):
                self.assertEqual(worker.local_model(path), path)
                (root / "model.safetensors").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "pinned revision"):
                    worker.local_model(path)
                (root / "model.safetensors").unlink()
                (root / "other").write_bytes(b"fixture")
                (root / "model.safetensors").symlink_to(root / "other")
                with self.assertRaises(ValueError):
                    worker.local_model(path)

    def test_unsafe_clip_and_oversize_text(self):
        with tempfile.TemporaryDirectory() as path:
            os.chmod(path, 0o700)
            outside = Path(path) / "outside"
            outside.write_bytes(b"0" * 12800)
            os.symlink(outside, Path(path) / "clip.raw")
            with self.assertRaises(OSError):
                worker.read_clip(path)
            (Path(path) / "clip.raw").unlink()
            class Large:
                def transcribe(self, **kwargs):
                    return {"text": "a" * 4097}
            with patch.object(worker, "read_clip", return_value=[]):
                in_r, in_w = os.pipe()
                out_r, out_w = os.pipe()
                try:
                    worker.send_frame(in_w, b"T", struct.pack("<Q", 7))
                    os.close(in_w)
                    in_w = -1
                    worker.run(Large(), path, in_r, out_w)
                    self.assertEqual(worker.read_frame(out_r), b"Y")
                    self.assertEqual(worker.read_frame(out_r), b"E" + struct.pack("<Q", 7) + b"transcription failed [response: ValueError]")
                finally:
                    os.close(in_r)
                    if in_w >= 0:
                        os.close(in_w)
                    os.close(out_r)
                    os.close(out_w)


if __name__ == "__main__":
    if "--clip-dir" in sys.argv:
        fake_child()
    else:
        unittest.main()
