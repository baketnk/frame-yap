"""Strict backend manifests and offline, shared pinned-model verification.

No model/runtime dependencies are imported. The caller opts in to hashing; neither
loading a manifest nor checking local files fetches anything or starts a worker.
The wire protocol and launcher argument contract are described in the manifest.
"""

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat

DEFAULT_MANIFEST_DIR = Path(__file__).resolve().parents[2] / "assets" / "backends"
_ID = re.compile(r"[a-z][a-z0-9_-]{0,47}\Z", re.ASCII)
_HASH = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
_MAX_MANIFEST = 65536
_MAX_FILE = 16 * 1024 ** 3
_ALLOWED_TOKENS = ("{model_dir}", "{clip_dir}", "{threads}")


class ManifestError(ValueError):
    """Invalid, unsafe or unsupported manifest (never silently skip one)."""


def _keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"duplicate manifest field: {key}")
        result[key] = value
    return result


def _fields(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ManifestError(f"invalid {label} fields")


def _text(value, label):
    if not isinstance(value, str) or not value or len(value) > 1024 or any(
        ord(c) < 32 or ord(c) == 127 for c in value
    ):
        raise ManifestError(f"invalid {label}")
    return value


def _relative(value, label):
    _text(value, label)
    if (value.startswith("/") or "\\" in value or any(
        not component or component in (".", "..") for component in value.split("/")
    )):
        raise ManifestError(f"unsafe {label}")
    return value


@dataclass(frozen=True)
class ModelFile:
    path: str
    size: int
    sha256: str


@dataclass(frozen=True)
class Backend:
    id: str
    display_name: str
    launcher: dict
    source: str
    revision: str
    files: tuple[ModelFile, ...]
    attribution: str
    license_id: str
    license_text: str
    cpu: str
    gpu: str

    @property
    def total_bytes(self):
        return sum(file.size for file in self.files)

    def description(self):
        return {"id": self.id, "display_name": self.display_name,
                "total_bytes": self.total_bytes, "launcher": self.launcher,
                "source": self.source, "revision": self.revision,
                "attribution": self.attribution,
                "license": {"id": self.license_id, "text": self.license_text},
                "requirements": {"cpu": self.cpu, "gpu": self.gpu}}


def _parse(value):
    _fields(value, ("schema", "id", "display_name", "launcher", "model",
                    "attribution", "license", "requirements"), "manifest")
    if type(value["schema"]) is not int or value["schema"] != 1:
        raise ManifestError("unsupported manifest schema")
    ident = value["id"]
    if not isinstance(ident, str) or not _ID.fullmatch(ident):
        raise ManifestError("invalid backend id")
    display = _text(value["display_name"], "display_name")
    launcher = value["launcher"]
    _fields(launcher, ("type", "path", "arguments", "protocol"), "launcher")
    if launcher["type"] not in ("python", "executable") or launcher["protocol"] != "frameyap-worker-v1":
        raise ManifestError("unsupported launcher or protocol")
    _relative(launcher["path"], "launcher path")
    args = launcher["arguments"]
    if not isinstance(args, list) or not 1 <= len(args) <= 32:
        raise ManifestError("invalid launcher arguments")
    for arg in args:
        _text(arg, "launcher argument")
        # Never interpolate arbitrary formatting or execute through a shell.
        if "{" in arg or "}" in arg:
            if arg not in _ALLOWED_TOKENS:
                raise ManifestError("unsupported launcher argument placeholder")
    if "{model_dir}" not in args or "{clip_dir}" not in args:
        raise ManifestError("launcher must accept model_dir and clip_dir")
    model = value["model"]
    _fields(model, ("source", "revision", "files"), "model")
    source = _text(model["source"], "model source")
    if not source.startswith("https://"):
        raise ManifestError("model source must be HTTPS")
    revision = _text(model["revision"], "model revision")
    files = model["files"]
    if not isinstance(files, list) or not 1 <= len(files) <= 64:
        raise ManifestError("invalid model file list")
    parsed = []
    names = set()
    for item in files:
        _fields(item, ("path", "size", "sha256"), "model file")
        name = _relative(item["path"], "model file path")
        size = item["size"]
        if name in names or type(size) is not int or not 0 < size <= _MAX_FILE or not isinstance(item["sha256"], str) or not _HASH.fullmatch(item["sha256"]):
            raise ManifestError("duplicate or invalid pinned model file")
        names.add(name)
        parsed.append(ModelFile(name, size, item["sha256"]))
    license_info = value["license"]
    _fields(license_info, ("id", "text"), "license")
    requirements = value["requirements"]
    _fields(requirements, ("cpu", "gpu"), "requirements")
    return Backend(ident, display, launcher, source, revision, tuple(parsed),
                   _text(value["attribution"], "attribution"),
                   _text(license_info["id"], "license id"),
                   _text(license_info["text"], "license text"),
                   _text(requirements["cpu"], "CPU requirements"),
                   _text(requirements["gpu"], "GPU requirements"))


def load_backends(manifest_dir=DEFAULT_MANIFEST_DIR):
    """Return all manifests keyed by ID; reject invalid/symlinked/duplicate entries."""
    root = Path(manifest_dir)
    if root.is_symlink() or not root.is_dir():
        raise ManifestError("manifest directory missing or unsafe")
    result = {}
    for file in sorted(root.iterdir()):
        if file.suffix != ".json":
            continue
        try:
            fd = os.open(file, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(fd, "rb") as handle:
                info = os.fstat(handle.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_size > _MAX_MANIFEST:
                    raise ManifestError("manifest file oversized or unsafe")
                raw = handle.read(_MAX_MANIFEST + 1)
            if len(raw) > _MAX_MANIFEST:
                raise ManifestError("manifest file oversized or unsafe")
            data = json.loads(raw.decode("utf-8"), object_pairs_hook=_keys)
            backend = _parse(data)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise ManifestError(f"invalid manifest: {file.name}") from error
        if file.stem != backend.id or backend.id in result:
            raise ManifestError("manifest filename and backend id must match")
        result[backend.id] = backend
    if not result:
        raise ManifestError("no backend manifests found")
    return result


def check_file(directory, item):
    """Return (reason, filename); reason None means the pinned file verifies."""
    root = Path(directory)
    if not root.is_absolute():
        return "model_dir_not_absolute", item.path
    if not root.exists() and not root.is_symlink():
        return "directory_missing", item.path
    if root.is_symlink() or not root.is_dir():
        return "unsafe_model_dir", item.path
    flags_dir = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    try:
        fd = os.open(root, flags_dir)
    except OSError:
        return "unsafe_model_dir", item.path
    try:
        components = item.path.split("/")
        for component in components[:-1]:
            new_fd = os.open(component, flags_dir, dir_fd=fd)
            os.close(fd)
            fd = new_fd
        file_fd = os.open(components[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        try:
            before = os.fstat(file_fd)
            if not stat.S_ISREG(before.st_mode):
                return "unsafe_file", item.path
            if before.st_size != item.size:
                return "size_mismatch", item.path
            digest = hashlib.sha256()
            while chunk := os.read(file_fd, 1024 * 1024):
                digest.update(chunk)
            after = os.fstat(file_fd)
            if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns) or digest.hexdigest() != item.sha256:
                return "hash_mismatch", item.path
            return None, None
        finally:
            os.close(file_fd)
    except FileNotFoundError:
        # A dangling symlink must not be mistaken for an absent file.
        try:
            os.stat(components[-1], dir_fd=fd, follow_symlinks=False)
        except FileNotFoundError:
            return "missing_files", item.path
        except OSError:
            return "unsafe_file", item.path
        return "unsafe_file", item.path
    except OSError:
        return "unsafe_file", item.path
    finally:
        os.close(fd)


def check_model(backend, directory):
    """Offline status for the *direct* pinned model directory (never a download)."""
    status = backend.description()
    status.update(state="installed_verified", reason=None, file=None)
    missing = None
    for item in backend.files:
        reason, name = check_file(directory, item)
        if reason in ("directory_missing", "missing_files"):
            if missing is None:
                missing = reason, name
        elif reason is not None:
            status.update(state="invalid", reason=reason, file=name)
            return status
    if missing:
        status.update(state="not_installed", reason=missing[0], file=missing[1])
    return status
