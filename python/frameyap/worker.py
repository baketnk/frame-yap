"""Persistent local Redux worker. No imports of model dependencies until explicit start.

Wire protocol: unsigned LE32 payload length (max 65536), type byte, unsigned
LE64 request id for T/R/E; Y (ready) and F (load failure) have no id. T refers
to the fixed clip.raw in the private directory. R/E carry <=4096 UTF-8 bytes.
Only one T may be outstanding. No stdout other than frames; stderr is suppressed
by default, or captured privately with explicit --advanced-debug. This module never captures audio or delivers input.
"""

import argparse
import os
from pathlib import Path
import stat
import struct
import sys
import traceback

MAX_FRAME = 65536
MAX_TEXT = 4096
MIN_SAMPLES = 3200
MAX_SAMPLES = 320000
try:
    from .model_files import DEFAULT_MANIFEST_DIR, ManifestError, check_model, load_backends
except ImportError:  # direct executable script
    from model_files import DEFAULT_MANIFEST_DIR, ManifestError, check_model, load_backends


class LocalModelError(ValueError):
    """Missing, incomplete or wrong pinned local weights (never a download cue)."""


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


def local_model(path, manifest_dir=DEFAULT_MANIFEST_DIR):
    """Check Redux against its manifest before loading any inference libraries."""
    try:
        backend = load_backends(manifest_dir)["redux"]
    except (ManifestError, KeyError) as error:
        raise LocalModelError("pinned Redux manifest missing or invalid") from error
    result = check_model(backend, path)
    if result["state"] != "installed_verified":
        raise LocalModelError("pinned local Redux model weights missing, unsafe or mismatched: " + result["reason"])
    return str(Path(path))


def load_model(path, threads, manifest_dir=DEFAULT_MANIFEST_DIR):
    # Set before importing moondream/torch/huggingface dependencies, even when
    # invoked directly without the native adapter.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_DATASETS_OFFLINE"] = "1"
    for key in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
        os.environ[key] = str(threads)
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    directory = local_model(path, manifest_dir)
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


def safe_request_error(stage, error):
    """Only fixed labels: never stringify exceptions, paths, audio or transcripts."""
    if stage not in ("audio", "inference", "response"):
        stage = "unknown"
    # Use built-in categories, not an arbitrary exception class name supplied by
    # a model/runtime. Subclasses are reduced to their safe built-in category.
    categories = (MemoryError, ImportError, OSError, UnicodeError, TypeError,
                  ValueError, KeyError, IndexError, RuntimeError)
    category = next((kind.__name__ for kind in categories if isinstance(error, kind)), "Exception")
    return f"transcription failed [{stage}: {category}]".encode("ascii")


def run(model, directory, input_fd=0, output_fd=1, advanced_debug=False):
    private_dir(directory)
    send_frame(output_fd, b"Y")
    while True:
        frame = read_frame(input_fd)
        if frame is None:
            return
        if len(frame) != 9 or frame[0:1] != b"T":
            raise ValueError("invalid request")
        request_id = frame[1:9]
        stage = "audio"
        try:
            audio = read_clip(directory)
            if advanced_debug:
                print(f"request {int.from_bytes(request_id, 'little')}: audio samples={len(audio)} rate=16000", file=sys.stderr, flush=True)
            stage = "inference"
            result = model.transcribe(audio=audio, sample_rate=16000)
            stage = "response"
            text = result["text"]
            if not isinstance(text, str):
                raise TypeError("invalid model response")
            encoded = text.encode("utf-8", errors="strict")
            if len(encoded) > MAX_TEXT:
                raise ValueError("transcript exceeds 4096 bytes")
            if advanced_debug:
                print(f"request {int.from_bytes(request_id, 'little')}: transcript={text!r}", file=sys.stderr, flush=True)
            send_frame(output_fd, b"R", request_id + encoded)
        except Exception as error:
            if advanced_debug:
                print(f"request {int.from_bytes(request_id, 'little')}: failure stage={stage}", file=sys.stderr, flush=True)
                traceback.print_exc(file=sys.stderr)
            # Only fixed stage/category labels cross IPC. Exception messages and
            # tracebacks may contain private audio/text and remain suppressed.
            send_frame(output_fd, b"E", request_id + safe_request_error(stage, error))


def main(argv=None):
    parser = argparse.ArgumentParser(description="FrameYap local offline Redux worker")
    parser.add_argument("--model", required=True)
    parser.add_argument("--manifest-dir", type=Path, default=DEFAULT_MANIFEST_DIR)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--clip-dir", required=True)
    parser.add_argument("--advanced-debug", action="store_true",
                        help="log full exceptions/runtime output and transcripts to stderr; may contain private speech")
    args = parser.parse_args(argv)
    # Dispatcher uses this explicit protocol preference for the pinned Redux
    # launcher, rather than adding flags to arbitrary backend commands.
    args.advanced_debug = args.advanced_debug or os.environ.get("FRAMEYAP_ADVANCED_DEBUG") == "1"
    if not 1 <= args.threads <= 64:
        parser.error("threads must be 1..64")
    # Native libraries sometimes print directly to fd 1. Keep those bytes out of
    # the framed channel, not merely Python's sys.stdout wrapper.
    protocol_fd = os.dup(1)
    os.set_inheritable(protocol_fd, False)
    if args.advanced_debug:
        # Native parent supplies a bounded private diagnostic sink. Model/native
        # stdout must still never corrupt the duplicated framed protocol fd.
        os.dup2(2, 1)
        print("FrameYap advanced debugging ON: private speech/text/paths may be logged; no raw clip archive.", file=sys.stderr, flush=True)
    else:
        with open(os.devnull, "wb") as null:
            os.dup2(null.fileno(), 1)
            os.dup2(null.fileno(), 2)
    try:
        try:
            private_dir(args.clip_dir)
        except Exception:
            send_frame(protocol_fd, b"F", b"M")
            return 1
        try:
            model = load_model(args.model, args.threads, args.manifest_dir)
        except LocalModelError:
            if args.advanced_debug: traceback.print_exc(file=sys.stderr)
            send_frame(protocol_fd, b"F", b"M")
            return 1
        except ImportError:
            if args.advanced_debug: traceback.print_exc(file=sys.stderr)
            send_frame(protocol_fd, b"F", b"I")
            return 1
        except Exception:
            if args.advanced_debug: traceback.print_exc(file=sys.stderr)
            send_frame(protocol_fd, b"F", b"D")
            return 1
        if args.advanced_debug:
            print("Local model ready", file=sys.stderr, flush=True)
        run(model, args.clip_dir, output_fd=protocol_fd, advanced_debug=args.advanced_debug)
        return 0
    finally:
        os.close(protocol_fd)


if __name__ == "__main__":
    sys.exit(main())
