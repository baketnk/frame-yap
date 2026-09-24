#!/usr/bin/env python3
"""Opt-in local CPU/IPC benchmark of a supplied nonprivate WAV. Never captures or types."""
import argparse
import array
import os
from pathlib import Path
import select
import statistics
import struct
import subprocess
import tempfile
import time
import wave


def receive(fd, timeout):
    end = time.monotonic() + timeout
    def exact(count):
        data = bytearray()
        while len(data) < count:
            remaining = end - time.monotonic()
            if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                raise TimeoutError("worker deadline")
            chunk = os.read(fd, count - len(data))
            if not chunk:
                raise RuntimeError("worker exited")
            data.extend(chunk)
        return bytes(data)
    size, = struct.unpack("<I", exact(4))
    if not 1 <= size <= 65536:
        raise ValueError("bad frame")
    return exact(size)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--python", required=True)
    p.add_argument("--worker", type=Path, required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--wav", type=Path, required=True, help="nonprivate PCM16 mono 16kHz WAV, <=20s")
    p.add_argument("--threads", type=int, choices=[2, 4], default=2)
    p.add_argument("--repeats", type=int, default=5)
    p.add_argument("--show-text", action="store_true", help="explicitly print this nonprivate fixture's transcript")
    args = p.parse_args()
    if not 1 <= args.repeats <= 20:
        p.error("repeats must be 1..20")
    with wave.open(str(args.wav), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000) or not 3200 <= wav.getnframes() <= 320000:
            p.error("expected 0.2..20s mono PCM16 at 16kHz")
        raw = wav.readframes(wav.getnframes())
    pcm = array.array("f", (x[0] / 32768.0 for x in struct.iter_unpack("<h", raw)))
    if __import__("sys").byteorder != "little":
        pcm.byteswap()
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        p.error("XDG_RUNTIME_DIR required")
    with tempfile.TemporaryDirectory(prefix="frameyap-benchmark-", dir=runtime) as directory:
        start = time.monotonic()
        child = subprocess.Popen([args.python, str(args.worker.absolute()), "--model", str(args.model.absolute()),
                                  "--threads", str(args.threads), "--clip-dir", directory],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            ready = receive(child.stdout.fileno(), 120)
            if ready != b"Y":
                raise RuntimeError(f"worker warmup rejected: {ready!r}")
            print(f"load_seconds={time.monotonic() - start:.3f}", flush=True)
            times = []
            for ident in range(1, args.repeats + 1):
                clip = Path(directory) / "clip.raw"
                fd = os.open(clip, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "wb") as output:
                    output.write(pcm.tobytes())
                start = time.monotonic()
                child.stdin.write(struct.pack("<IcQ", 9, b"T", ident))
                child.stdin.flush()
                reply = receive(child.stdout.fileno(), 60)
                elapsed = time.monotonic() - start
                if len(reply) < 9 or reply[:1] != b"R" or struct.unpack("<Q", reply[1:9])[0] != ident:
                    raise RuntimeError("worker transcription failed or wrong correlation")
                text = reply[9:].decode("utf-8")
                clip.unlink()
                times.append(elapsed)
                print(f"request={ident} seconds={elapsed:.3f} bytes={len(reply)-9}", flush=True)
                if args.show_text and ident == 1:
                    print(f"public_fixture_transcript={text}", flush=True)
            print(f"threads={args.threads} median_seconds={statistics.median(times):.3f} max_seconds={max(times):.3f}")
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            child.stdout.close()


if __name__ == "__main__":
    main()
