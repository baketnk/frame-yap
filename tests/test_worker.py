"""Hardware-free tests, and a deliberately fake child for native IPC tests.

Run: python3 -m unittest discover -s tests -p test_worker.py
Native fixture: python3 tests/test_worker.py --model ok --threads 2 --clip-dir DIR
"""
import os
from pathlib import Path
import struct
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
        clip = Path(args.clip_dir) / "clip.raw"
        assert clip.stat().st_size == 3200 * 4
        ident = msg[1:9]
        if args.model == "stale":
            ident = struct.pack("<Q", struct.unpack("<Q", ident)[0] + 1)
        if args.model == "oversized-frame":
            os.write(1, struct.pack("<I", 65537))
            continue
        if args.model == "request-error" and requests == 1:
            worker.send_frame(1, b"E", ident + b"transcription failed")
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
                    self.assertEqual(worker.read_frame(out_r), b"E" + struct.pack("<Q", 7) + b"transcription failed")
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
