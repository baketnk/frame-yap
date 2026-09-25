"""Manifest-driven local worker dispatcher. One verified model, one child, no shell/network.

The child speaks frameyap-worker-v1; this process checks framing and correlation
before forwarding Y/T/R/E. The native parent owns the private clip and deadlines.
"""
import argparse
import os
import signal
from pathlib import Path
import subprocess
import sys

try:
    from .model_files import load_backends, check_model, ManifestError
    from .worker import read_frame, send_frame, private_dir
except ImportError:
    from model_files import load_backends, check_model, ManifestError
    from worker import read_frame, send_frame, private_dir


def launcher_command(backend, root, python, model_dir, clip_dir, threads):
    """Return argv, never a shell string. Manifest paths are relative to release root."""
    relative = backend.launcher["path"]
    root = Path(root).resolve(strict=True)
    target = root / relative
    if not target.is_file() or target.is_symlink() or not target.resolve().is_relative_to(root):
        raise ValueError("unsafe or missing backend launcher")
    replacements = {"{model_dir}": str(model_dir), "{clip_dir}": str(clip_dir),
                    "{threads}": str(threads)}
    args = [replacements.get(a, a) for a in backend.launcher["arguments"]]
    if backend.launcher["type"] == "python":
        return [python, str(target), *args]
    if not os.access(target, os.X_OK):
        raise ValueError("backend executable is not executable")
    return [str(target), *args]


def serve(backend, root, python, model_dir, clip_dir, threads, advanced_debug=False,
          input_fd=0, output_fd=1):
    private_dir(clip_dir)
    if check_model(backend, model_dir)["state"] != "installed_verified":
        send_frame(output_fd, b"F", b"M")
        return 1
    try:
        argv = launcher_command(backend, root, python, model_dir, clip_dir, threads)
    except (OSError, ValueError):
        send_frame(output_fd, b"F", b"I")
        return 1
    env = os.environ.copy()
    env.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_DATASETS_OFFLINE="1",
               TOKENIZERS_PARALLELISM="false", CUDA_VISIBLE_DEVICES="",
               OMP_NUM_THREADS=str(threads), MKL_NUM_THREADS=str(threads),
               OPENBLAS_NUM_THREADS=str(threads), PYTHONDONTWRITEBYTECODE="1")
    # Protocol opt-in only for our known Redux worker. Arbitrary backend
    # launchers need not accept --advanced-debug or know this environment key.
    env["FRAMEYAP_ADVANCED_DEBUG"] = "1" if (advanced_debug and backend.id == "redux" and
        backend.launcher["type"] == "python" and backend.launcher["path"] == "python/frameyap/worker.py") else "0"
    # A child cannot inherit input-delivery authority. stdout is exclusively frames.
    try:
        child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=None if advanced_debug else subprocess.DEVNULL,
                                 env=env, close_fds=True)
    except OSError:
        send_frame(output_fd, b"F", b"I")
        return 1
    def stop_child(_signal, _frame):
        # Native shutdown may SIGKILL this dispatcher after a short grace period.
        # Reap the direct model child before exiting, never leave inference running.
        child.kill()
        child.wait()
        raise SystemExit(1)

    old_term = signal.signal(signal.SIGTERM, stop_child)
    try:
        ready = read_frame(child.stdout.fileno())
        if ready is None or ready[:1] == b"F":
            send_frame(output_fd, b"F", ready[1:2] if ready and ready[1:2] in (b"M", b"I") else b"D")
            return 1
        if ready != b"Y":
            send_frame(output_fd, b"F", b"D")
            return 1
        send_frame(output_fd, b"Y")
        while True:
            request = read_frame(input_fd)
            if request is None:
                return 0
            if len(request) != 9 or request[:1] != b"T":
                raise ValueError("invalid worker request")
            send_frame(child.stdin.fileno(), b"T", request[1:])
            reply = read_frame(child.stdout.fileno())
            if (reply is None or len(reply) < 9 or len(reply) > 4105 or
                    reply[:1] not in (b"R", b"E") or reply[1:9] != request[1:]):
                raise ValueError("invalid backend response")
            send_frame(output_fd, reply[:1], reply[1:])
    finally:
        # An EOF/invalid protocol must not block cleanup if a child ignores TERM.
        # Native also kills the dedicated owned worker group after its grace period.
        try:
            child.stdin.close()
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            else:
                child.wait()
        finally:
            child.stdout.close()
            signal.signal(signal.SIGTERM, old_term)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--manifest-dir", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--python", required=True)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--clip-dir", required=True, type=Path)
    parser.add_argument("--threads", required=True, type=int)
    parser.add_argument("--advanced-debug", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.threads <= 64:
        parser.error("threads must be 1..64")
    # Never let native/model library stdout corrupt the framed channel.
    protocol = os.dup(1)
    with open(os.devnull, "wb") as null:
        os.dup2(2 if args.advanced_debug else null.fileno(), 1)
        if not args.advanced_debug:
            os.dup2(null.fileno(), 2)
    try:
        try:
            backend = load_backends(args.manifest_dir)[args.backend]
        except (ManifestError, KeyError):
            send_frame(protocol, b"F", b"M")
            return 1
        return serve(backend, args.root, args.python, args.model, args.clip_dir,
                     args.threads, args.advanced_debug, output_fd=protocol)
    finally:
        os.close(protocol)


if __name__ == "__main__":
    sys.exit(main())
