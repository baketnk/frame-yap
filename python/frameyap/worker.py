"""Persistent local Redux worker. No imports of model dependencies until explicit start.

Wire protocol: unsigned LE32 payload length (max 65536), type byte, unsigned
LE64 request id for T/R/E; Y (ready) and F (load failure) have no id. T refers
to the fixed clip.raw in the private directory. R/E carry <=4096 UTF-8 bytes.
Only one T may be outstanding. No stdout other than frames; stderr is suppressed
by the native launcher. This module never captures audio or delivers input.
"""

import argparse
import hashlib
import os
from pathlib import Path
import stat
import struct
import sys

MAX_FRAME = 65536
MAX_TEXT = 4096
MIN_SAMPLES = 3200
MAX_SAMPLES = 320000
try:
    from .model_files import REVISION, FILES
except ImportError:  # direct executable script
    from model_files import REVISION, FILES


def read_exact(fd, count):
    parts = bytearray()
    while len(parts) < count:
        part = os.read(fd, count - len(parts))
        if not part:
            if not parts:
                return None
            raise ValueError("truncated worker frame")
        parts.extend(part)
    return bytes(parts)


def read_frame(fd):
    header = read_exact(fd, 4)
    if header is None:
        return None
    size, = struct.unpack("<I", header)
    if not 1 <= size <= MAX_FRAME:
        raise ValueError("invalid frame length")
    payload = read_exact(fd, size)
    if payload is None:
        raise ValueError("truncated frame")
    return payload


def send_frame(fd, kind, data=b""):
    payload = kind + data
    if len(payload) > MAX_FRAME:
        raise ValueError("oversize response")
    frame = struct.pack("<I", len(payload)) + payload
    while frame:
        written = os.write(fd, frame)
        if written <= 0:
            raise OSError("closed worker pipe")
        frame = frame[written:]


def private_dir(path):
    st = os.lstat(path)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077:
        raise ValueError("clip directory must be private and owned by current user")


def local_model(path):
    """Require a real local directory with weight file(s), never a hub identifier."""
    root = Path(path)
    if not root.is_absolute() or not root.is_dir() or root.is_symlink():
        raise ValueError("absolute local model directory required")
    for name, (size, expected) in FILES.items():
        file = root / name
        if not file.is_file() or file.is_symlink() or file.stat().st_size != size:
            raise ValueError("pinned local Redux model weights missing or incomplete")
        digest = hashlib.sha256()
        with file.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            raise ValueError("local Redux model does not match pinned revision")
    return str(root)


def load_model(path, threads):
    # Set before importing moondream/torch/huggingface dependencies, even when
    # invoked directly without the native adapter.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_DATASETS_OFFLINE"] = "1"
    for key in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
        os.environ[key] = str(threads)
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    directory = local_model(path)
    import torch
    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)
    import moondream as md  # noqa: explicit lazy import
    return md.photon("moondream/parakeet-redux", model_path=directory,
                     device="cpu", cpu_threads=threads)


def read_clip(directory):
    # No symlinks; the parent wrote an exclusive, mode-0600 file. Keep private
    # audio out of logs and do not follow a swapped filename.
    fd = os.open(os.path.join(directory, "clip.raw"), os.O_RDONLY | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077:
            raise ValueError("unsafe clip")
        if not MIN_SAMPLES * 4 <= st.st_size <= MAX_SAMPLES * 4 or st.st_size % 4:
            raise ValueError("invalid clip length")
        raw = read_exact(fd, st.st_size)
        if raw is None or os.read(fd, 1):
            raise ValueError("clip changed during read")
    finally:
        os.close(fd)
    import numpy as np  # deferred until the explicit request
    pcm = np.frombuffer(raw, dtype="<f4")
    if not np.isfinite(pcm).all():
        raise ValueError("invalid PCM")
    return pcm


def run(model, directory, input_fd=0, output_fd=1):
    private_dir(directory)
    send_frame(output_fd, b"Y")
    while True:
        frame = read_frame(input_fd)
        if frame is None:
            return
        if len(frame) != 9 or frame[0:1] != b"T":
            raise ValueError("invalid request")
        request_id = frame[1:9]
        try:
            result = model.transcribe(audio=read_clip(directory), sample_rate=16000)
            text = result["text"]
            if not isinstance(text, str):
                raise ValueError("invalid model response")
            encoded = text.encode("utf-8", errors="strict")
            if len(encoded) > MAX_TEXT:
                raise ValueError("transcript exceeds 4096 bytes")
            send_frame(output_fd, b"R", request_id + encoded)
        except Exception:
            # Model exceptions may include audio or transcripts. Do not log or
            # forward them to the overlay; request-local failure only.
            send_frame(output_fd, b"E", request_id + b"transcription failed")


def main(argv=None):
    parser = argparse.ArgumentParser(description="FrameYap local offline Redux worker")
    parser.add_argument("--model", required=True)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--clip-dir", required=True)
    args = parser.parse_args(argv)
    if not 1 <= args.threads <= 64:
        parser.error("threads must be 1..64")
    # Native libraries sometimes print directly to fd 1. Keep those bytes out of
    # the framed channel, not merely Python's sys.stdout wrapper.
    protocol_fd = os.dup(1)
    os.set_inheritable(protocol_fd, False)
    with open(os.devnull, "wb") as null:
        os.dup2(null.fileno(), 1)
        os.dup2(null.fileno(), 2)
    try:
        try:
            private_dir(args.clip_dir)
            # The full hash validation occurs exactly once in load_model.
            if not Path(args.model).is_dir():
                raise ValueError("missing model")
        except Exception:
            send_frame(protocol_fd, b"F", b"M")
            return 1
        try:
            model = load_model(args.model, args.threads)
        except Exception:
            send_frame(protocol_fd, b"F", b"D")
            return 1
        run(model, args.clip_dir, output_fd=protocol_fd)
        return 0
    finally:
        os.close(protocol_fd)


if __name__ == "__main__":
    sys.exit(main())
