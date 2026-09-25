#!/bin/sh
# FrameYap pinned release installer. No implicit version, downloads, or runtime launch.
set -eu
json=false
for arg in "$@"; do [ "$arg" = --json ] && json=true; done
bootstrap_error() {
    if [ "$json" = true ]; then
        printf '{"ok":false,"code":"bootstrap","message":"%s","exit_code":1}\n' "$1"
    else
        printf 'frameyap installer: %s\n' "$1" >&2
    fi
    exit 1
}
command -v python3 >/dev/null 2>&1 || bootstrap_error 'missing prerequisite: python3'
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' || {
    bootstrap_error 'Python 3.12+ required for bootstrap only'
}
# Keep the original invocation's stdin on fd 3; the heredoc supplies only Python code.
# The payload uses fd 3 for prompts only if both input and output are real TTYs.
exec python3 - "$@" 3<&0 <<'PY'
"""FrameYap installer implementation. Embedded verbatim in install.sh for piped installs."""
import argparse
import contextlib
from datetime import datetime
import io
import importlib.util
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import signal
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import quote
import urllib.request

KEY = "local.frameyap.overlay"
# The pinned release this installer belongs to (set in the release commit that
# is tagged vRELEASE_VERSION); empty in a development tree.
RELEASE_VERSION = ""
MARKER = "# FrameYap managed launcher v1\n"
ARCHIVE_LIMIT = 12 * 1024**3
MEMBER_LIMIT = 50000
VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.[1-9][0-9]{11}\Z")
DIGEST_RE = re.compile(r"[a-fA-F0-9]{64}\Z")
CONFIG_DEFAULTS = {
    "font": "",
    "input_priority": "normal",
    "advanced_debug": False,
    "auto_insert": False,
    "close_mic_when_idle": False,
    "backend": "redux",
    "lock_layout": False,
    "clock_24h": False,
    "date_format": "mdy",
    "quick_inputs": ["/new", "/questions", "/help"],
    "wrist": {"x": 0, "y": 0.18, "z": 0.089, "width": 0.30, "roll_degrees": 0},
    "theme": {"background": "#0c101b", "card": "#141c2b", "ink": "#e6f0f9",
              "muted": "#97adc1", "accent": "#1ff0a4", "warning": "#ff6e87",
              "frame_start": "#1fff91", "frame_end": "#1f70ff"},
    "gradient": {"enabled": True, "period_seconds": 30, "strength": 0.12},
    "buttons": {"left_grip": "/user/hand/left/input/grip",
                "right_grip": "/user/hand/right/input/grip",
                "ptt": "/user/hand/right/input/x", "cancel": "/user/hand/right/input/b",
                "insert": "/user/hand/right/input/a", "enter": "",
                "quick_chat": "/user/hand/right/input/y"},
}
# Explicit --install-runtime only. CPU Torch comes from PyTorch's CPU index first;
# an unconstrained PyPI resolve selects CUDA/NVIDIA wheels. moondream 2.4.0 pins
# kestrel 0.8.0, kestrel-native 0.1.8 and kestrel-kernels 0.7.0.
RUNTIME_TORCH = "torch==2.8.0"
RUNTIME_TORCH_INDEX = "https://download.pytorch.org/whl/cpu"
RUNTIME_REQUIREMENT = "moondream==2.4.0"
RUNTIME_DOWNLOAD = "about 200 MB of prebuilt wheels (CPU Torch is about 100 MB), roughly 1-1.5 GB on disk; no compilation"
RUNTIME_PYTHON_RANGE = ((3, 10), (3, 14))  # moondream allows <3.15; CPU Torch 2.8.0 wheels stop at 3.13
RUNTIME_NAME_RE = re.compile(r"cpu-[0-9]{14}\Z")
RUNTIME_VERIFY = (
    "import importlib.metadata as m, torch\n"
    "assert torch.version.cuda is None, 'CUDA Torch was installed; CPU build required'\n"
    "import moondream\n"
    "print('moondream', m.version('moondream'), 'kestrel', m.version('kestrel'), 'torch', torch.__version__)\n")
COLOR_RE = re.compile(r"#[0-9a-fA-F]{6}\Z")
BUTTON_RE = re.compile(r"/user/hand/(left|right)/input/[A-Za-z0-9_]+\Z")
BACKEND_RE = re.compile(r"[a-z][a-z0-9_-]{0,47}\Z", re.ASCII)


def fail(message):
    raise ValueError(message)


def json_atomic(path, value):
    atomic_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


def atomic_write(path, content, mode=0o600):
    fd, name = tempfile.mkstemp(prefix=".frameyap-", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def config_path():
    xdg = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg) if xdg and Path(xdg).is_absolute() else Path.home() / ".config"
    return base / "frameyap/config.json"


def paths_config_path():
    return config_path().parent / "paths.conf"


def set_runtime_python(python):
    """Point the managed launcher at `python`; other paths.conf lines are kept verbatim."""
    path = paths_config_path()
    if "\n" in str(python) or not python.is_absolute():
        fail(f"unsafe runtime path: {python!r}")
    owned_dir(path.parent)
    if path.is_symlink():
        fail(f"refusing symlink paths config: {path}")
    lines = (path.read_text().splitlines() if path.exists() else
             ["# FrameYap literal paths (python=/abs, model=/abs); never shell code"])
    previous = [line[len("python="):] for line in lines if line.startswith("python=")]
    kept = [line for line in lines if not line.startswith("python=")]
    atomic_write(path, ("\n".join(kept + [f"python={python}"]) + "\n").encode())
    return previous[-1] if previous else None


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON config key: {key}")
        result[key] = value
    return result


def normalized_config(data):
    # Invalid individual values reset to defaults. Unknown/retired properties
    # are removed; the exact original bytes are backed up before any rewrite.
    if not isinstance(data, dict):
        data = {}
    fixed = {"font": data.get("font") if isinstance(data.get("font"), str) else ""}
    priority = data.get("input_priority", "normal")
    fixed["input_priority"] = priority if priority in ("normal", "experimental") else "normal"
    debug = data.get("advanced_debug", False)
    fixed["advanced_debug"] = debug if type(debug) is bool else False
    automatic = data.get("auto_insert", False)
    fixed["auto_insert"] = automatic if type(automatic) is bool else False
    close_mic = data.get("close_mic_when_idle", False)
    fixed["close_mic_when_idle"] = close_mic if type(close_mic) is bool else False
    backend = data.get("backend", "redux")
    fixed["backend"] = backend if isinstance(backend, str) and BACKEND_RE.fullmatch(backend) else "redux"
    layout = data.get("lock_layout", False)
    fixed["lock_layout"] = layout if type(layout) is bool else False
    clock = data.get("clock_24h", False)
    fixed["clock_24h"] = clock if type(clock) is bool else False
    date = data.get("date_format", "mdy")
    fixed["date_format"] = date if date in ("off", "mdy", "dmy", "iso") else "mdy"
    quick = data.get("quick_inputs", CONFIG_DEFAULTS["quick_inputs"])
    fixed["quick_inputs"] = quick if (isinstance(quick, list) and 1 <= len(quick) <= 6
        and all(isinstance(item, str) and 1 <= len(item) <= 64 and item.strip(" ")
                and all(32 <= ord(ch) <= 126 for ch in item) for item in quick)) else CONFIG_DEFAULTS["quick_inputs"].copy()
    source = data.get("wrist")
    source = source if isinstance(source, dict) else {}
    fixed["wrist"] = {}
    for name, default in CONFIG_DEFAULTS["wrist"].items():
        value = source.get(name, default)
        limit = (.15, .6) if name == "width" else (-180, 180) if name == "roll_degrees" else (-.3, .3)
        fixed["wrist"][name] = value if type(value) in (int, float) and limit[0] <= value <= limit[1] else default
    gradient = data.get("gradient")
    gradient = gradient if isinstance(gradient, dict) else {}
    fixed["gradient"] = {}
    for name, default in CONFIG_DEFAULTS["gradient"].items():
        value = gradient.get(name, default)
        if name == "enabled":
            valid = type(value) is bool
        else:
            low, high = (5, 300) if name == "period_seconds" else (0, 0.3)
            valid = type(value) in (int, float) and low <= value <= high and math.isfinite(value)
        fixed["gradient"][name] = value if valid else default
    for section in ("theme", "buttons"):
        source = data.get(section)
        source = source if isinstance(source, dict) else {}
        fixed[section] = {}
        for name, default in CONFIG_DEFAULTS[section].items():
            value = source.get(name, default)
            valid = (isinstance(value, str) and (COLOR_RE.fullmatch(value) if section == "theme"
                     else (not value or BUTTON_RE.fullmatch(value))))
            fixed[section][name] = value if valid else default
    if "quick_chat" not in (data.get("buttons") if isinstance(data.get("buttons"), dict) else {}) and fixed["buttons"]["enter"] == "/user/hand/right/input/y":
        fixed["buttons"]["enter"] = ""  # former default Y is now quick chat
    paths = [value for value in fixed["buttons"].values() if value]
    if len(paths) != len(set(paths)):
        fixed["buttons"] = CONFIG_DEFAULTS["buttons"].copy()
    size = lambda: len((json.dumps(fixed, indent=2, sort_keys=True) + "\n").encode())
    if size() > 4096:
        fixed["font"] = ""
    if size() > 4096:
        fixed["buttons"] = CONFIG_DEFAULTS["buttons"].copy()
        fixed["quick_inputs"] = CONFIG_DEFAULTS["quick_inputs"].copy()
    return fixed


def repair_user_config():
    path = config_path()
    owned_dir(path.parent.parent)
    owned_dir(path.parent)
    if path.is_symlink() or (path.exists() and not path.is_file()):
        fail(f"refusing foreign config path: {path}")
    original = None
    parsed = None
    if path.exists():
        if path.stat().st_size > 65536:
            fail(f"config too large to safely back up: {path}")
        original = path.read_bytes()
        try:
            if len(original) > 4096:  # native config reader has the same bound
                fail("config exceeds native 4096-byte limit")
            parsed = json.loads(original.decode("utf-8"), object_pairs_hook=unique_pairs)
        except (ValueError, UnicodeError):
            pass
    fixed = normalized_config(parsed)
    if original is not None and parsed == fixed:
        return  # Keep valid user formatting, permissions and custom values.
    if original is not None:
        fd, backup = tempfile.mkstemp(prefix="config.json.backup-", dir=path.parent)
        with os.fdopen(fd, "wb") as stream:
            stream.write(original)
            stream.flush()
            os.fsync(stream.fileno())
        print(f"Preserved previous FrameYap config at {backup}")
    json_atomic(path, fixed)


def owned_dir(path):
    if path.is_symlink():
        fail(f"refusing symlink directory: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        fail(f"not a directory: {path}")


def check_host():
    if sys.platform != "linux" or platform.machine().lower() not in ("aarch64", "arm64"):
        fail("release requires Linux AArch64 (ARM64); no cross-architecture install")
    if platform.libc_ver()[0] != "glibc":
        fail("release requires glibc Linux; libc compatibility still requires native testing")


def digest_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def check_digest(value):
    if not DIGEST_RE.fullmatch(value):
        fail("--sha256 must be a 64-character hex SHA-256")
    return value.lower()


def check_version(value):
    if not VERSION_RE.fullmatch(value):
        fail("version must be numeric MAJOR.MINOR.YYYYMMDDHHMM (no leading zeros, v prefix or git suffix)")
    try:
        datetime.strptime(value.rsplit(".", 1)[1], "%Y%m%d%H%M")
    except ValueError:
        fail("invalid UTC date/time in version")
    return value


def release_name(version):
    return f"frameyap-{version}-linux-aarch64.tar.gz"


def download(url, destination):
    subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https",
                    "--proto-redir", "=https", "--tlsv1.2", "--max-filesize",
                    str(4096 if url.endswith(".sha256") else ARCHIVE_LIMIT),
                    "--output", str(destination), url], check=True)


def verify_members(archive):
    seen = set()
    total = 0
    count = 0
    for member in archive:
        count += 1
        if count > MEMBER_LIMIT:
            fail("too many archive members")
        name = member.name
        parts = name.rstrip("/").split("/")
        if (not name or name.startswith("/") or len(name) > 1024 or len(parts) > 32
                or any(x in ("", ".", "..") for x in parts)
                or "\\" in name or "\x00" in name):
            fail(f"unsafe archive path: {name!r}")
        if name in seen:
            fail(f"duplicate archive member: {name}")
        seen.add(name)
        if not (member.isfile() or member.isdir()):
            fail(f"archive links and special files forbidden: {name}")
        if member.size < 0 or member.size > ARCHIVE_LIMIT:
            fail("oversized archive member")
        if name == "release.json" and member.size > 8192:
            fail("oversized release metadata")
        total += member.size
        if total > ARCHIVE_LIMIT:
            fail("archive uncompressed limit exceeded")
        if (parts[0] not in ("release.json", "bin", "lib", "assets", "python", "scripts", "runtime", "model", "fonts", "licenses")
                or (parts[0] == "release.json" and (len(parts) != 1 or not member.isfile()))
                or (len(parts) == 1 and parts[0] != "release.json" and not member.isdir())):
            fail(f"unexpected archive path: {name}")


def extract(archive_path, destination):
    with tarfile.open(archive_path, "r:gz") as archive:
        verify_members(archive)
    with tarfile.open(archive_path, "r:gz") as archive:
        for member in archive:
            target = destination.joinpath(*member.name.rstrip("/").split("/"))
            if member.isdir():
                owned_dir(target)
            else:
                owned_dir(target.parent)
                # Never follow a pre-existing link, including one created by the archive.
                with target.open("xb") as out, archive.extractfile(member) as source:
                    shutil.copyfileobj(source, out, 1024 * 1024)
                # Retain executable helpers/shared libraries; discard all other mode bits.
                target.chmod(0o700 if member.mode & 0o111 else 0o600)


def validate_payload(root, version, without_model=False, installed=False):
    meta = json.loads((root / "release.json").read_text())
    if not isinstance(meta, dict):
        fail("invalid release metadata")
    if meta.get("schema") != 1 or meta.get("version") != version or meta.get("arch") != "linux-aarch64":
        fail("release metadata version/architecture/schema mismatch")
    if meta.get("runtime") not in ("bundled-cpu-python", "external-authorized-python") or not isinstance(meta.get("model_revision"), str) or not meta["model_revision"]:
        fail("missing runtime/model revision declaration")
    has_model = meta.get("includes_model")
    if not isinstance(has_model, bool) or (not installed and not has_model and (root / "model").exists()):
        fail("inconsistent model declaration")
    if has_model and not (root / "model").is_dir() and not (installed and without_model and not (root / "model").exists()):
        fail("missing declared model")
    if meta["runtime"] == "external-authorized-python" and (root / "runtime").exists():
        fail("external runtime payload must not include a runtime")
    for name in ("bin/frameyap", "bin/install.sh", "runtime/bin/python3", "python/frameyap/worker.py", "assets/actions.json", "fonts/font.ttf"):
        if name == "runtime/bin/python3" and meta["runtime"] == "external-authorized-python":
            continue
        if name == "bin/install.sh" and installed and not VERSION_RE.fullmatch(version):
            continue  # legacy installed release, before a managed UI child existed
        if not (root / name).is_file():
            fail(f"missing payload file: {name}")
    for name in ("lib", "fonts"):
        if not (root / name).is_dir() or not any((root / name).iterdir()):
            fail(f"missing payload directory: {name}")
    return meta


def selected(root, link):
    path = root / link
    if not path.is_symlink():
        if path.exists():
            fail(f"refusing foreign selection path: {path}")
        return None
    value = os.readlink(path)
    if not re.fullmatch(r"versions/[A-Za-z0-9][A-Za-z0-9._-]{0,95}", value):
        fail(f"invalid {link} selection")
    if not (root / value).is_dir():
        fail(f"broken {link} selection")
    return value


def select(root, link, target):
    path = root / link
    temp = root / ("." + link + "-" + str(os.getpid()))
    try:
        os.symlink(target, temp)
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def desired_launcher(root, legacy=False, pinned_font=False, prior_launcher=False):
    import shlex
    q = lambda path: shlex.quote(str(path))
    base = root / "current"
    flags = ["--assets", base / "assets", "--worker", base / "python/frameyap/worker.py"]
    if pinned_font or legacy:
        flags[2:2] = ["--font", base / "fonts/font.ttf"]  # exact previously managed launchers
    args = " ".join(q(item) for item in flags)
    # Parse two literal absolute paths, never source/eval this user-owned file.
    # Environment overrides remain useful for a one-off explicit launch.
    config = ('' if legacy else
              'CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/frameyap/paths.conf\n'
              'CONFIG_PYTHON= CONFIG_MODEL=\n'
              'if [ -f "$CONFIG" ] && [ ! -L "$CONFIG" ]; then\n'
              '  while IFS= read -r line || [ -n "$line" ]; do\n'
              '    case "$line" in\n'
              '      python=/*) CONFIG_PYTHON=${line#python=};;\n'
              '      model=/*) CONFIG_MODEL=${line#model=};;\n'
              '    esac\n'
              '  done < "$CONFIG"\n'
              'fi\n')
    python_default = (q(base / "runtime/bin/python3") if legacy else
                      '${CONFIG_PYTHON:-' + q(base / "runtime/bin/python3") + '}')
    model_default = (q(base / "model") if legacy else
                     '${CONFIG_MODEL:-' + q(base / "model") + '}')
    check_font = ' --font ' + q(base / "fonts/font.ttf") if legacy or pinned_font else ''
    preamble = ("#!/bin/sh\n" + MARKER + 'export PYTHONDONTWRITEBYTECODE=1\n'
                + f'export FRAMEYAP_INSTALL_ROOT={q(root)}\n'
                + f'export LD_LIBRARY_PATH={q(base / "lib")}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}\n'
                + config
                + f'PYTHON=${{FRAMEYAP_PYTHON:-{python_default}}}\n'
                + f'MODEL=${{FRAMEYAP_MODEL:-{model_default}}}\n'
                + 'if [ "$#" -eq 0 ]; then set -- --run; fi\n'
                + 'case "$1" in\n')
    # These are only for byte-for-byte migration of earlier managed wrappers.
    if legacy or pinned_font or prior_launcher:
        return (preamble
                + '  --run)\n'
                + '    if [ -n "${GAMESCOPE_SOCKET:-}" ]; then set -- "$@" --socket "$GAMESCOPE_SOCKET"; fi\n'
                + f"    shift; exec {q(base / 'bin/frameyap')} --run {args} --python \"$PYTHON\" --model \"$MODEL\" \"$@\";;\n"
                + f"  --check-overlay|--check-controls) mode=$1; shift; exec {q(base / 'bin/frameyap')} \"$mode\" --assets {q(base / 'assets')}{check_font} \"$@\";;\n"
                + f"  *) exec {q(base / 'bin/frameyap')} \"$@\";;\n"
                + 'esac\n').encode()
    # Scan the original argv without shifting or evaluating it. Prepend each
    # absent default in reverse order, preserving explicit options (including
    # duplicates) for the binary's strict parser to handle.
    return (preamble
            + '  --run)\n'
            + '    shift\n'
            + '    has_assets= has_worker= has_python= has_model= has_socket=\n'
            + '    for arg in "$@"; do\n'
            + '      case "$arg" in\n'
            + '        --assets) has_assets=1;;\n'
            + '        --worker) has_worker=1;;\n'
            + '        --python) has_python=1;;\n'
            + '        --model) has_model=1;;\n'
            + '        --socket) has_socket=1;;\n'
            + '      esac\n'
            + '    done\n'
            + '    if [ -z "$has_socket" ] && [ -n "${GAMESCOPE_SOCKET:-}" ]; then\n'
            + '      set -- --socket "$GAMESCOPE_SOCKET" "$@"\n'
            + '    fi\n'
            + '    if [ -z "$has_model" ]; then set -- --model "$MODEL" "$@"; fi\n'
            + '    if [ -z "$has_python" ]; then set -- --python "$PYTHON" "$@"; fi\n'
            + f'    if [ -z "$has_worker" ]; then set -- --worker {q(base / "python/frameyap/worker.py")} "$@"; fi\n'
            + f'    if [ -z "$has_assets" ]; then set -- --assets {q(base / "assets")} "$@"; fi\n'
            + f"    exec {q(base / 'bin/frameyap')} --run \"$@\";;\n"
            + f"  --check-overlay|--check-controls) mode=$1; shift; exec {q(base / 'bin/frameyap')} \"$mode\" --assets {q(base / 'assets')} \"$@\";;\n"
            + f"  *) exec {q(base / 'bin/frameyap')} \"$@\";;\n"
            + 'esac\n').encode()


def desired_manifest(launcher):
    return {"source": "builtin", "applications": [{"app_key": KEY, "launch_type": "binary",
            "binary_path_linux": str(launcher), "binary_path_linux_arm": str(launcher),
            "is_dashboard_overlay": True,
            "strings": {"en_us": {"name": "FrameYap"}}}]}


def desktop_path(root):
    return root.parent / "applications/frameyap.desktop"


def desired_desktop(launcher):
    # Desktop Entry Exec quotes are not shell quotes; escape reserved characters.
    executable = (str(launcher).replace("\\", "\\\\").replace('"', '\\"')
                  .replace("$", "\\$").replace("`", "\\`").replace("%", "%%"))
    return ("[Desktop Entry]\nType=Application\nName=FrameYap\n"
            f'Exec="{executable}"\nTerminal=false\nCategories=Utility;\n').encode()


def check_owned_file(path, expected, alternatives=()):
    if path.is_symlink():
        fail(f"refusing foreign symlink: {path}")
    if path.exists():
        if not path.is_file():
            fail(f"refusing foreign path: {path}")
        data = path.read_bytes()
        if data != expected and data not in alternatives:
            fail(f"refusing to replace modified/foreign file: {path}")


def check_wrappers(root, launcher):
    manifest = root / "frameyap.vrmanifest"
    desired = (json.dumps(desired_manifest(launcher), sort_keys=True, indent=2) + "\n").encode()
    # Accept only exact earlier managed scripts for migration.
    check_owned_file(launcher, desired_launcher(root),
                     (desired_launcher(root, prior_launcher=True), desired_launcher(root, pinned_font=True),
                      desired_launcher(root, legacy=True)))
    check_owned_file(manifest, desired)
    check_owned_file(desktop_path(root), desired_desktop(launcher))
    return manifest, desired


def install_files(root, installed, launcher):
    owned_dir(launcher.parent)
    manifest, desired = check_wrappers(root, launcher)
    desktop = desktop_path(root)
    owned_dir(desktop.parent)
    atomic_write(launcher, desired_launcher(root), 0o755)
    atomic_write(manifest, desired)
    atomic_write(desktop, desired_desktop(launcher))


def receipt(path):
    marker = path / ".archive-sha256"
    if marker.is_symlink() or not marker.is_file() or not DIGEST_RE.fullmatch(marker.read_text().strip()):
        fail(f"refusing unowned installation directory: {path}")
    return marker.read_text().strip()


def inventory(path):
    return sorted(p.relative_to(path).as_posix() for p in path.rglob("*")
                  if p.relative_to(path).parts[0] != "model"
                  and p.relative_to(path).as_posix() not in (".archive-sha256", ".installed-files.json", ".install-options.json"))


def check_inventory(path):
    index = path / ".installed-files.json"
    if not index.is_file() or index.is_symlink():
        fail(f"missing installation inventory: {path}")
    declared = json.loads(index.read_text())
    if not isinstance(declared, list) or inventory(path) != declared:
        fail(f"refusing to remove untracked files in: {path}")
    for p in path.rglob("*"):
        if p.is_symlink() and p.relative_to(path).parts[0] != "model":
            fail(f"refusing link inside installation: {p}")


def installed_choice(path):
    receipt(path)
    options = path / ".install-options.json"
    if not options.is_file() or options.is_symlink():
        fail(f"missing installation options: {path}")
    choice = json.loads(options.read_text())
    if not isinstance(choice, dict) or set(choice) != {"without_model"} or not isinstance(choice["without_model"], bool):
        fail(f"invalid installation options: {path}")
    return choice["without_model"]


class ManifestMismatch(ValueError):
    """The installed manifest does not match the UI's selected metadata."""


def manifest_digest(path):
    """Hash the exact installed ID.json bytes, never a symlink or directory."""
    import stat
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 65536:
            fail("installed backend manifest is oversized or unsafe")
        raw = stream.read(65537)
        if len(raw) > 65536:
            fail("installed backend manifest is oversized or unsafe")
    return hashlib.sha256(raw).hexdigest()


def installed_backend(root, backend_id):
    current = selected(root, "current")
    if not current:
        fail("no installed release; install a verified archive before provisioning models")
    version = root / current
    installed_choice(version)
    check_inventory(version)
    module_path = version / "python/frameyap/model_files.py"
    if module_path.is_symlink() or not module_path.is_file():
        fail("installed release has no shared backend manifest verifier")
    if module_path.stat().st_size > 262144:
        fail("installed backend verifier exceeds size limit")
    spec = importlib.util.spec_from_file_location("frameyap_installed_model_files", module_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    # Executing compiled bytes avoids __pycache__ inside the inventory-protected
    # installed release; a machine-readable installer must not mutate it.
    exec(compile(module_path.read_bytes(), str(module_path), "exec"), module.__dict__)
    manifests = module.load_backends(version / "assets/backends")
    if backend_id not in manifests:
        fail(f"unknown backend: {backend_id}; available: {', '.join(sorted(manifests))}")
    # load_backends enforces filename == manifest id. Hash only that selected file,
    # not all manifests or their parsed/reformatted representation.
    digest = manifest_digest(version / "assets/backends" / (backend_id + ".json"))
    return module, manifests[backend_id], digest


def check_expected_manifest(args, installed_sha):
    if args.expected_manifest_sha256 and args.expected_manifest_sha256.lower() != installed_sha:
        raise ManifestMismatch(f"installed {args.backend}.json manifest SHA-256 mismatch: "
                               f"expected {args.expected_manifest_sha256.lower()}, got {installed_sha}")


def model_target(args, root):
    if args.model_dir:
        if not args.model_dir.is_absolute():
            fail("--model-dir must be an absolute local path")
        return args.model_dir
    return root / "models" / args.backend


@contextlib.contextmanager
def model_download_cancellation(message="model installation cancelled"):
    # Install-model runs on the main thread. Raising unwinds the owned temp's
    # finally block; unlike SIGKILL this does not leave a partial download.
    def terminate(_signal, _frame):
        raise ValueError(message)

    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        yield
    finally:
        signal.signal(signal.SIGTERM, previous)


def install_model(args, root):
    module, backend, manifest_sha = installed_backend(root, args.backend)
    check_expected_manifest(args, manifest_sha)  # under .model.lock, before model mkdir/network
    dest = model_target(args, root)
    # No credentials, redirects to HTTP, symlinks or overwrite of mismatched files.
    for ancestor in (dest, *dest.parents):
        if ancestor.is_symlink():
            fail(f"refusing symlink in model directory path: {ancestor}")
    owned_dir(dest)
    if not backend.source.startswith("https://"):
        fail("model source must be HTTPS")
    for item in backend.files:
        target = dest / item.path
        owned_dir(target.parent)
        reason, _ = module.check_file(dest, item)
        if reason is None:
            if args.json:
                print(json.dumps({"ok": True, "event": "model_file", "file": item.path, "state": "verified"}), flush=True)
            continue
        if reason not in ("missing_files",):
            fail(f"refusing mismatched or unsafe model file: {target} ({reason})")
        url = f"{backend.source}/resolve/{quote(backend.revision, safe='')}/{quote(item.path, safe='/')}"
        if args.json:
            print(json.dumps({"ok": True, "event": "model_file", "file": item.path, "state": "downloading", "bytes": item.size}), flush=True)
        with model_download_cancellation():
            fd, temp = tempfile.mkstemp(prefix=".download-", dir=target.parent)
            try:
                with os.fdopen(fd, "wb") as output:
                    # The shared verifier is used for both pre-existing and downloaded files.
                    request = urllib.request.Request(url, headers={"User-Agent": "frameyap-installer"})
                    with urllib.request.urlopen(request, timeout=60) as response:
                        if response.geturl().split(":", 1)[0] != "https":
                            fail("model URL redirected away from HTTPS")
                        total = 0
                        while chunk := response.read(1024 * 1024):
                            total += len(chunk)
                            if total > item.size:
                                fail(f"model download exceeded pinned size: {item.path}")
                            output.write(chunk)
                    output.flush()
                    os.fsync(output.fileno())
                temporary = type(item)(Path(temp).name, item.size, item.sha256)
                if module.check_file(target.parent, temporary)[0] is not None:
                    fail(f"pinned SHA-256/size mismatch: {item.path}")
                if target.exists() or target.is_symlink():
                    fail(f"model destination changed during download: {target}")
                os.replace(temp, target)
                if args.json:
                    print(json.dumps({"ok": True, "event": "model_file", "file": item.path, "state": "verified"}), flush=True)
            finally:
                Path(temp).unlink(missing_ok=True)
    state = module.check_model(backend, dest)
    if state["state"] != "installed_verified":
        fail(f"model did not verify: {state['reason']}")
    if not args.json:
        print(f"Backend {backend.id} model verified at {dest}; license {backend.license_id}. Configure runtime/model paths explicitly for launch.")


def runtime_step(args, name, command):
    if args.json:
        print(json.dumps({"ok": True, "event": "runtime_step", "step": name}), flush=True)
    else:
        print(f"[runtime] {name}", flush=True)
    # Tool output goes to stderr so --json stdout stays one event per line.
    subprocess.run(command, check=True, stdout=sys.stderr, stdin=subprocess.DEVNULL)


def install_runtime(args, root):
    low, high = RUNTIME_PYTHON_RANGE
    if not low <= sys.version_info[:2] < high:
        fail("the runtime needs Python 3.10-3.13 (CPU Torch 2.8.0 wheels); found " + platform.python_version())
    if importlib.util.find_spec("venv") is None or importlib.util.find_spec("ensurepip") is None:
        fail("python3 cannot create a virtual environment (venv/ensurepip missing); no packages installed")
    runtimes = root / "runtimes"
    owned_dir(runtimes)
    target = runtimes / datetime.now().strftime("cpu-%Y%m%d%H%M%S")
    if target.exists() or target.is_symlink():
        fail(f"runtime directory already exists: {target}")
    python = target / "bin/python3"
    # --isolated ignores user pip config/env indexes; binary-only avoids compilers.
    pip = [str(python), "-m", "pip", "--isolated", "--disable-pip-version-check", "--no-input",
           "install", "--only-binary=:all:"]
    try:
        with model_download_cancellation("runtime installation cancelled"):
            runtime_step(args, "create virtual environment", [sys.executable, "-m", "venv", str(target)])
            runtime_step(args, "install CPU Torch", pip + ["--index-url", RUNTIME_TORCH_INDEX, RUNTIME_TORCH])
            constraints = target / "frameyap-constraints.txt"
            constraints.write_text(RUNTIME_TORCH + "\n")
            runtime_step(args, "install moondream/Kestrel", pip + ["--constraint", str(constraints), RUNTIME_REQUIREMENT])
            runtime_step(args, "verify CPU runtime imports", [str(python), "-c", RUNTIME_VERIFY])
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        raise
    previous = set_runtime_python(python)
    # Superseded managed runtimes are removed; anything else is never touched.
    for item in runtimes.iterdir():
        if item != target and item.is_dir() and not item.is_symlink() and RUNTIME_NAME_RE.fullmatch(item.name):
            shutil.rmtree(item)
    if not args.json:
        print(f"CPU Python runtime installed at {target}; {paths_config_path()} now has python={python}.")
        if previous and not previous.startswith(str(runtimes) + "/"):
            print(f"Previous python={previous} was replaced; restore it in paths.conf to switch back.")


def source_preflight(args):
    """Read-only checks before the installer creates its install root."""
    source = Path(args.source).expanduser().resolve(strict=True)
    if not source.is_dir() or not (source / "CMakeLists.txt").is_file():
        fail("--source must name a local FrameYap source directory")
    for script in ("scripts/install-preflight.sh", "scripts/stage-native.py", "scripts/package-release.py"):
        if not (source / script).is_file() or (source / script).is_symlink():
            fail(f"source missing regular packaging script: {script}")
    sdk = Path(args.openvr_root).expanduser().resolve(strict=True)
    if not (sdk / "headers/openvr.h").is_file():
        fail("--openvr-root must contain headers/openvr.h")
    inputs = {}
    for name in ("openvr_library", "openvr_license", "sdl_library", "sdl_license"):
        path = Path(getattr(args, name)).expanduser().resolve(strict=True)
        if not path.is_file():
            fail(f"--{name.replace('_', '-')} must be a local file")
        inputs[name] = str(path)
    for tool in ("cmake", "c++", "pkg-config", "wayland-scanner"):
        if not shutil.which(tool):
            fail(f"source prerequisite missing: {tool}; no download/package install attempted")
    preflight = subprocess.run(["sh", str(source / "scripts/install-preflight.sh"), "--source"],
                              capture_output=True, text=True, check=False)
    if preflight.returncode:
        fail(f"source preflight failed (exit {preflight.returncode}): {preflight.stderr[-2000:]}")
    for dep in ("sdl3", "wayland-client", "xcb", "freetype2", "vulkan"):
        if subprocess.run(["pkg-config", "--exists", dep], check=False).returncode:
            fail(f"source dependency missing: {dep}; provide local native dependencies")
    return source, sdk, inputs


def source_model_revision(source):
    """Use the explicit source tree's pinned manifest, not a frozen installer hash."""
    manifest = source / "assets/backends/redux.json"
    if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size > 65536:
        fail("source missing safe pinned Redux backend manifest")
    data = json.loads(manifest.read_text())
    if not isinstance(data, dict) or data.get("id") != "redux" or not isinstance(data.get("model"), dict):
        fail("invalid source Redux backend manifest")
    revision = data["model"].get("revision")
    if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{40}", revision):
        fail("invalid pinned Redux model revision in source manifest")
    return revision


def source_archive(args, work):
    """Build only explicitly supplied local sources/dependencies in private work dir."""
    source, sdk, inputs = source_preflight(args)
    revision = source_model_revision(source)
    build = work / "build"
    stage = work / "stage"
    output = work / "out"
    output.mkdir(mode=0o700)
    commands = [
        ["cmake", "-S", str(source), "-B", str(build), "-DFRAMEYAP_NATIVE=ON",
         f"-DOPENVR_ROOT={sdk}", f"-DFRAMEYAP_VERSION={args.version}"],
        ["cmake", "--build", str(build)],
        [sys.executable, str(source / "scripts/stage-native.py"), "--build", str(build),
         "--destination", str(stage), *[x for name in inputs for x in ("--" + name.replace("_", "-"), inputs[name])]],
        [sys.executable, str(source / "scripts/package-release.py"), "--stage", str(stage),
         "--output", str(output), "--arch", "linux-aarch64", "--version", args.version,
         "--model-revision", revision, "--external-runtime"],
    ]
    for cmd in commands:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode:
            fail(f"source command failed ({cmd[0]}, exit {result.returncode}): {result.stderr[-2000:]}")
    archive = output / release_name(args.version)
    if not archive.is_file():
        fail("source packaging did not produce the expected release archive")
    return archive, digest_file(archive)


def do_install(args, root, launcher):
    version = check_version(args.version)
    versions = root / "versions"
    owned_dir(versions)
    current = selected(root, "current")
    selected(root, "previous")
    # Refuse foreign wrappers/launcher directories before installing a new version.
    owned_dir(launcher.parent)
    check_wrappers(root, launcher)
    with tempfile.TemporaryDirectory(prefix=".download-", dir=root) as td:
        if args.source:
            archive, expected = source_archive(args, Path(td))
            do_download = False
        elif args.archive:
            archive = Path(args.archive).expanduser().resolve(strict=True)
            expected = check_digest(args.sha256)
            do_download = False
        else:
            do_download = True
            archive = None
        if do_download:
            name = release_name(version)
            url = f"https://github.com/{args.repo}/releases/download/{quote('v' + version)}/{name}"
            archive = Path(td) / name
            sidecar = Path(td) / (name + ".sha256")
            download(url + ".sha256", sidecar)
            if sidecar.stat().st_size > 4096:
                fail("oversized SHA-256 sidecar")
            line = sidecar.read_text().strip()
            match = re.fullmatch(r"([a-fA-F0-9]{64})  " + re.escape(name), line)
            if not match:
                fail("invalid versioned SHA-256 sidecar")
            expected = match.group(1).lower()
            download(url, archive)
        if archive.stat().st_size > ARCHIVE_LIMIT:
            fail("compressed archive exceeds limit")
        if not do_download:
            local_copy = Path(td) / "local-archive.tar.gz"
            shutil.copyfile(archive, local_copy)
            archive = local_copy
        if digest_file(archive) != expected:
            fail("archive SHA-256 mismatch")
        target = versions / version
        if target.exists() or target.is_symlink():
            if target.is_symlink() or receipt(target) != expected:
                fail("same version already installed with different archive digest")
            if installed_choice(target) != args.without_model:
                fail("same version already installed with different --without-model choice")
            check_inventory(target)
            validate_payload(target, version, args.without_model, installed=True)
        else:
            temp = Path(tempfile.mkdtemp(prefix=".staging-", dir=versions))
            try:
                extract(archive, temp)
                validate_payload(temp, version)
                if args.without_model and (temp / "model").exists():
                    shutil.rmtree(temp / "model")
                json_atomic(temp / ".installed-files.json", inventory(temp))
                json_atomic(temp / ".install-options.json", {"without_model": args.without_model})
                (temp / ".archive-sha256").write_text(expected + "\n")
                os.replace(temp, target)
            finally:
                if temp.exists():
                    shutil.rmtree(temp)
        repair_user_config()
        install_files(root, target, launcher)
        if current == f"versions/{version}":
            print(f"FrameYap {version}: already installed (same digest); wrappers verified")
            return
        if current and current != f"versions/{version}":
            select(root, "previous", current)
        select(root, "current", f"versions/{version}")
        print(f"FrameYap {version} installed. OpenVR registration is NOT automatic; see docs/packaging.md.")
        if json.loads((target / "release.json").read_text())["runtime"] == "external-authorized-python":
            print("ASR runtime is NOT included. Run install.sh --install-runtime --yes to pip-install it, or supply one with FRAMEYAP_PYTHON/--python or paths.conf.")


def uninstall(root, launcher):
    current = selected(root, "current")
    previous = selected(root, "previous")
    manifest = root / "frameyap.vrmanifest"
    desired = (json.dumps(desired_manifest(launcher), sort_keys=True, indent=2) + "\n").encode()
    check_owned_file(manifest, desired)
    check_owned_file(launcher, desired_launcher(root),
                     (desired_launcher(root, prior_launcher=True), desired_launcher(root, pinned_font=True),
                      desired_launcher(root, legacy=True)))
    desktop = desktop_path(root)
    check_owned_file(desktop, desired_desktop(launcher))
    versions = root / "versions"
    if versions.exists():
        if versions.is_symlink():
            fail("refusing symlink versions directory")
        for item in versions.iterdir():
            if not item.is_dir() or item.is_symlink() or item.name.startswith("."):
                fail(f"unexpected versions entry: {item}")
            choice = installed_choice(item)
            check_inventory(item)
            validate_payload(item, item.name, choice, installed=True)
            if (item / "model").exists() or (item / "model").is_symlink():
                saved = root / "saved-models"
                owned_dir(saved)
                target = saved / item.name
                if target.exists() or target.is_symlink():
                    fail(f"saved model already exists: {target}")
        for item in versions.iterdir():
            if (item / "model").exists() or (item / "model").is_symlink():
                os.replace(item / "model", root / "saved-models" / item.name)
            shutil.rmtree(item)
    for link in ("current", "previous"):
        (root / link).unlink(missing_ok=True)
    if launcher.exists():
        launcher.unlink()
    if manifest.exists():
        manifest.unlink()
    if desktop.exists():
        desktop.unlink()
    print("FrameYap removed; config, saved models and any runtimes/ directory preserved. "
          "OpenVR unregister acknowledgement was required.")


class UsageError(ValueError):
    pass


class InstallerParser(argparse.ArgumentParser):
    def error(self, message):
        raise UsageError(message)


def resolve_args(argv):
    parser = InstallerParser(prog="install.sh", description="User-local FrameYap installer; never launches the overlay")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--archive", help="local release archive (requires --sha256 and --version)")
    action.add_argument("--rollback", action="store_true")
    action.add_argument("--uninstall", action="store_true")
    action.add_argument("--install-model", action="store_true", help="explicit model provisioning for installed backend")
    action.add_argument("--install-runtime", action="store_true",
                        help="explicit pip install of the pinned CPU Python runtime into a user-local venv")
    parser.add_argument("--mode", choices=("binary", "source"), default="binary")
    parser.add_argument("--source", help="explicit local FrameYap source tree (source mode only)")
    parser.add_argument("--openvr-root", help="local OpenVR SDK root (source mode)")
    for name in ("openvr-library", "openvr-license", "sdl-library", "sdl-license"):
        parser.add_argument("--" + name, help="explicit local source packaging input")
    parser.add_argument("--sha256")
    parser.add_argument("--version", help="numeric MAJOR.MINOR.YYYYMMDDHHMM; GitHub tag is vVERSION")
    parser.add_argument("--repo", default="baketnk/frame-yap")
    parser.add_argument("--backend", default="redux")
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--expected-manifest-sha256", help="SHA-256 of selected installed backend ID.json bytes")
    parser.add_argument("--yes", action="store_true", help="explicit consent to network/model provisioning")
    parser.add_argument("--autolaunch", dest="autolaunch", action="store_true", default=None)
    parser.add_argument("--no-autolaunch", dest="autolaunch", action="store_false")
    parser.add_argument("--without-model", action="store_true")
    parser.add_argument("--print-plan", action="store_true", help="read-only plan; no lock, download or install")
    parser.add_argument("--json", action="store_true", help="one JSON result or error on stdout")
    parser.add_argument("--unregistered", action="store_true", help="acknowledge explicit OpenVR removal before uninstall")
    args = parser.parse_args(argv)
    if args.repo != "baketnk/frame-yap":
        parser.error("only the pinned baketnk/frame-yap release repository is supported")
    source_inputs = ("openvr_root", "openvr_library", "openvr_license", "sdl_library", "sdl_license")
    if args.mode == "source":
        if not args.source or any(not getattr(args, name) for name in source_inputs):
            parser.error("--mode source requires --source and explicit --openvr-root, --openvr-library, --openvr-license, --sdl-library, --sdl-license")
        if args.archive or args.sha256 or args.rollback or args.uninstall or args.install_model or args.install_runtime:
            parser.error("source mode cannot combine with binary archive or lifecycle operations")
    elif args.source or any(getattr(args, name) for name in source_inputs):
        parser.error("source inputs require --mode source")
    if args.archive and (not args.sha256 or not args.version):
        parser.error("--archive requires --sha256 and --version")
    if args.archive and not DIGEST_RE.fullmatch(args.sha256):
        parser.error("--sha256 must be a 64-character hex SHA-256")
    if not args.archive and args.sha256:
        parser.error("--sha256 only applies to --archive")
    if args.install_runtime and (args.version or args.without_model or args.autolaunch is not None or
                                 args.backend != "redux" or args.model_dir or args.expected_manifest_sha256):
        parser.error("--install-runtime is independent of release/model flags")
    if not (args.rollback or args.uninstall or args.install_model or args.install_runtime or args.version) \
            and RELEASE_VERSION and args.mode == "binary" and not args.archive:
        args.version = RELEASE_VERSION  # this installer's own pinned release, not a moving tag
    if not (args.rollback or args.uninstall or args.install_model or args.install_runtime) and not args.version:
        parser.error("--version VERSION is required; no moving/latest release")
    if args.uninstall != args.unregistered:
        parser.error("--uninstall requires --unregistered after explicit OpenVR unregister")
    if args.model_dir and not args.install_model:
        parser.error("--model-dir applies only to --install-model")
    if args.expected_manifest_sha256 is not None:
        if not args.install_model:
            parser.error("--expected-manifest-sha256 applies only to --install-model")
        if not DIGEST_RE.fullmatch(args.expected_manifest_sha256):
            parser.error("--expected-manifest-sha256 must be a 64-character hex SHA-256")
    if args.model_dir and not args.model_dir.is_absolute():
        parser.error("--model-dir must be an absolute local path")
    if args.backend != "redux" and not args.install_model:
        parser.error("--backend ID currently applies only to --install-model; select the active backend in FrameYap settings")
    if args.install_model and (args.without_model or args.version or args.archive or args.autolaunch is not None):
        parser.error("--install-model uses the installed version; cannot combine with archive/version/without-model/autolaunch")
    if (args.rollback or args.uninstall) and (args.version or args.without_model or args.autolaunch is not None or args.backend != "redux"):
        parser.error("lifecycle actions cannot combine with installation/model flags")
    if args.version:
        try:
            check_version(args.version)
        except ValueError as error:
            parser.error(str(error))
    return args


def plan(args):
    operation = ("uninstall" if args.uninstall else "rollback" if args.rollback else
                 "install-model" if args.install_model else "install-runtime" if args.install_runtime else "install")
    return {"operation": operation, "mode": args.mode, "version": args.version,
            "tag": "v" + args.version if args.version else None,
            "archive": str(args.archive) if args.archive else None,
            "source": str(args.source) if args.source else None,
            "backend": args.backend, "model_dir": str(args.model_dir) if args.model_dir else None,
            "expected_manifest_sha256": (args.expected_manifest_sha256.lower() if args.expected_manifest_sha256 else None),
            "without_model": args.without_model, "autolaunch": args.autolaunch,
            "network": bool(args.install_model or args.install_runtime or
                            (operation == "install" and not args.archive and not args.source)),
            "registration": ("explicit --register --autostart" if args.autolaunch is True else
                             "explicit --register (autostart off)" if args.autolaunch is False else "none")}


def main(argv=None):
    args = resolve_args(argv)
    if args.print_plan:
        result = plan(args)
        if args.install_model:
            data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share").expanduser().absolute()
            root = data / "frameyap"
            _, backend, manifest_sha = installed_backend(root, args.backend)
            check_expected_manifest(args, manifest_sha)
            result.update(model_dir=str(model_target(args, root)), model=backend.description(),
                          installed_manifest_sha256=manifest_sha)
        if args.install_runtime:
            data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share").expanduser().absolute()
            result.update(runtime_dir=str(data / "frameyap/runtimes"), base_python=sys.executable,
                          base_python_version=platform.python_version(),
                          packages=[RUNTIME_TORCH + " from " + RUNTIME_TORCH_INDEX, RUNTIME_REQUIREMENT + " from PyPI"],
                          download=RUNTIME_DOWNLOAD, paths_config=str(paths_config_path()))
        if args.json:
            print(json.dumps({"ok": True, "event": "plan", **result}, sort_keys=True))
        else:
            print(json.dumps(result, indent=2, sort_keys=True))
        return
    check_host()
    if args.mode == "source":
        source_preflight(args)
    if (not args.archive and not args.source and not args.rollback and not args.uninstall) and not args.yes:
        fail("network/model install requires explicit --yes (no stdin prompts); use --print-plan first")
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share").expanduser().absolute()
    root = data / "frameyap"
    launcher = Path.home() / ".local/bin/frameyap"
    owned_dir(root)
    # UI child model provisioning must work while the overlay holds .lock.
    # Models live outside versions and never replace a running executable.
    lock = root / (".model.lock" if args.install_model else ".lock")
    if lock.is_symlink():
        fail("refusing symlink lock")
    with lock.open("a+b") as fd:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail(f"FrameYap installer or application is running ({lock.name} held); try again later")
        other = root / ".model.lock"
        if not args.install_model and other.is_symlink():
            fail("refusing symlink model lock")
        with (contextlib.nullcontext() if args.install_model else other.open("a+b")) as model_fd:
            if model_fd is not None:
                try:
                    fcntl.flock(model_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    fail("model provisioning is running (.model.lock held); retry later")
            if args.uninstall:
                uninstall(root, launcher)
            elif args.rollback:
                owned_dir(root / "versions")
                current, previous = selected(root, "current"), selected(root, "previous")
                if not current or not previous:
                    fail("no previous installation to roll back to")
                check_wrappers(root, launcher)
                choice = installed_choice(root / previous)
                check_inventory(root / previous)
                validate_payload(root / previous, Path(previous).name, choice, installed=True)
                select(root, "current", previous)
                select(root, "previous", current)
                print(f"Rolled back to {previous}")
            elif args.install_model:
                install_model(args, root)
            elif args.install_runtime:
                install_runtime(args, root)
            else:
                do_install(args, root, launcher)
    # The native registration helper takes this same install lock. Never run it
    # until the installer has released its own lock; never initialize OpenVR by default.
    if args.autolaunch is not None:
        command = [str(launcher), "--register", str(root / "frameyap.vrmanifest")]
        if args.autolaunch:
            command.append("--autostart")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode:
            fail(f"files installed, but opt-in OpenVR autolaunch registration failed (exit {result.returncode}): {result.stderr[-1000:]}")
        print("OpenVR autolaunch " + ("enabled" if args.autolaunch else "disabled") + " by explicit request")


def ask_yes_no(question):
    while True:
        answer = input(question + " [y/n]: ").strip().lower()
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("Please answer y or n.")


def interactive_options():
    """Only an empty, actual terminal invocation offers a guided local choice.

    Returns one argv per step; each step is an ordinary flag-driven operation."""
    print("If you piped this script without reading it, you can press Ctrl+C now and read it first.")
    if RELEASE_VERSION:
        print(f"FrameYap installer for release v{RELEASE_VERSION} (github.com/baketnk/frame-yap).")
        if not ask_yes_no(f"Download and install FrameYap v{RELEASE_VERSION} from GitHub (checksum verified)?"):
            raise UsageError("download not approved; no installation started")
        return optional_steps([["--mode", "binary", "--version", RELEASE_VERSION, "--yes"]])
    print("FrameYap installer: choose binary (verified archive) or source (local build).")
    mode = input("Mode [binary/source]: ").strip().lower()
    if mode not in ("binary", "source"):
        raise UsageError("choose binary or source; no installation started")
    version = input("Numeric release version (MAJOR.MINOR.YYYYMMDDHHMM): ").strip()
    check_version(version)
    chosen = ["--mode", mode, "--version", version]
    if mode == "binary":
        archive = input("Local archive path (leave empty for pinned GitHub release): ").strip()
        if archive:
            chosen += ["--archive", archive, "--sha256", input("SHA-256 (64 hex digits): ").strip()]
        else:
            if not ask_yes_no("Download verified release v" + version + "?"):
                raise UsageError("download not approved; no installation started")
            chosen.append("--yes")
    else:
        for flag in ("source", "openvr-root", "openvr-library", "openvr-license", "sdl-library", "sdl-license"):
            chosen += ["--" + flag, input(flag + " local path: ").strip()]
    return optional_steps([chosen])


def optional_steps(steps):
    # Each download is described and separately approved; nothing is fetched on a default.
    if ask_yes_no("Download the Parakeet Redux speech model (about 180 MB, CC-BY-4.0) from Hugging Face?"):
        steps.append(["--install-model", "--backend", "redux", "--yes"])
    if ask_yes_no("Install the CPU Python runtime with pip (" + RUNTIME_DOWNLOAD + ")?"):
        steps.append(["--install-runtime", "--yes"])
    print("Equivalent commands:")
    for step in steps:
        print("  sh install.sh " + " ".join(shlex.quote(item) for item in step))
    return steps


def attended_input():
    """The real terminal for prompts, or None. Under `curl | sh` stdin (and the
    saved fd 3) is the pipe, so the controlling terminal is opened directly;
    piped data is never read as answers."""
    if sys.stdin.isatty():
        return sys.stdin
    try:
        if os.isatty(3):
            return os.fdopen(3, "r", closefd=False)
    except OSError:
        pass  # Direct Python entry point, no saved shell descriptor.
    try:
        return open("/dev/tty", "r")
    except OSError:
        return None  # no controlling terminal: never prompt


def cli(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    steps = [argv]
    if not argv and sys.stdout.isatty():
        # With `sh install.sh`, fd 0 is the embedded Python code, not the
        # invoking terminal. fd 3 retains original stdin (including a pipe).
        attended = attended_input()
        if attended is not None:
            try:
                original_stdin = sys.stdin
                try:
                    sys.stdin = attended
                    steps = interactive_options()
                finally:
                    sys.stdin = original_stdin
            except (ValueError, EOFError, KeyboardInterrupt) as exc:
                print(f"frameyap installer: {exc or 'cancelled'}", file=sys.stderr)
                return 2
    for step in steps:
        code = run_step(step)
        if code:
            return code
    return 0


def run_step(argv):
    structured = "--json" in argv
    try:
        if structured and "--print-plan" in argv:
            main(argv)  # already emits one JSON plan
        elif structured and ("--install-model" in argv or "--install-runtime" in argv):
            main(argv)  # streaming per-file JSON events for a UI child
            print(json.dumps({"ok": True, "event": "complete"}))
        elif structured:
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture):
                main(argv)
            print(json.dumps({"ok": True, "event": "complete", "messages": capture.getvalue().splitlines()}))
        else:
            main(argv)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError, json.JSONDecodeError, ImportError) as exc:
        code = 2 if isinstance(exc, UsageError) else 1
        if structured:
            print(json.dumps({"ok": False, "code": ("usage" if code == 2 else
                              "manifest_mismatch" if isinstance(exc, ManifestMismatch) else "operation_failed"),
                              "message": str(exc), "exit_code": code}))
        else:
            print(f"frameyap installer: {exc}", file=sys.stderr)
        return code


if __name__ == "__main__":
    sys.exit(cli())

PY
