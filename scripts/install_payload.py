"""FrameYap installer implementation. Embedded verbatim in install.sh for piped installs."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import quote

KEY = "local.frameyap.overlay"
MARKER = "# FrameYap managed launcher v1\n"
ARCHIVE_LIMIT = 12 * 1024**3
MEMBER_LIMIT = 50000
VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}\Z")
DIGEST_RE = re.compile(r"[a-fA-F0-9]{64}\Z")
CONFIG_DEFAULTS = {
    "font": "",
    "input_priority": "normal",
    "advanced_debug": False,
    "wrist": {"x": 0, "y": 0.18, "z": 0.089, "width": 0.30, "roll_degrees": 0},
    "theme": {"background": "#0c101b", "card": "#141c2b", "ink": "#e6f0f9",
              "muted": "#97adc1", "accent": "#1ff0a4", "warning": "#ff6e87",
              "frame_start": "#1fff91", "frame_end": "#1f70ff"},
    "buttons": {"left_grip": "/user/hand/left/input/grip",
                "right_grip": "/user/hand/right/input/grip",
                "ptt": "/user/hand/right/input/x", "cancel": "/user/hand/right/input/b",
                "insert": "/user/hand/right/input/a", "enter": "/user/hand/right/input/y"},
}
COLOR_RE = re.compile(r"#[0-9a-fA-F]{6}\Z")
BUTTON_RE = re.compile(r"/user/hand/(left|right)/input/[A-Za-z0-9_]+\Z")


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
    source = data.get("wrist")
    source = source if isinstance(source, dict) else {}
    fixed["wrist"] = {}
    for name, default in CONFIG_DEFAULTS["wrist"].items():
        value = source.get(name, default)
        limit = (.15, .6) if name == "width" else (-180, 180) if name == "roll_degrees" else (-.3, .3)
        fixed["wrist"][name] = value if type(value) in (int, float) and limit[0] <= value <= limit[1] else default
    for section in ("theme", "buttons"):
        source = data.get(section)
        source = source if isinstance(source, dict) else {}
        fixed[section] = {}
        for name, default in CONFIG_DEFAULTS[section].items():
            value = source.get(name, default)
            valid = (isinstance(value, str) and (COLOR_RE.fullmatch(value) if section == "theme"
                     else (not value or BUTTON_RE.fullmatch(value))))
            fixed[section][name] = value if valid else default
    paths = [value for value in fixed["buttons"].values() if value]
    if len(paths) != len(set(paths)):
        fixed["buttons"] = CONFIG_DEFAULTS["buttons"].copy()
    size = lambda: len((json.dumps(fixed, indent=2, sort_keys=True) + "\n").encode())
    if size() > 4096:
        fixed["font"] = ""
    if size() > 4096:
        fixed["buttons"] = CONFIG_DEFAULTS["buttons"].copy()
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
    if not VERSION_RE.fullmatch(value) or value in (".", ".."):
        fail("invalid release version/tag")
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
        if (parts[0] not in ("release.json", "bin", "lib", "assets", "python", "runtime", "model", "fonts", "licenses")
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
    for name in ("bin/frameyap", "runtime/bin/python3", "python/frameyap/worker.py", "assets/actions.json", "fonts/font.ttf"):
        if name == "runtime/bin/python3" and meta["runtime"] == "external-authorized-python":
            continue
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


def desired_launcher(root, legacy=False, pinned_font=False):
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
    return ("#!/bin/sh\n" + MARKER + 'export PYTHONDONTWRITEBYTECODE=1\n'
            + f'export FRAMEYAP_INSTALL_ROOT={q(root)}\n'
            + f'export LD_LIBRARY_PATH={q(base / "lib")}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}\n'
            + config
            + f'PYTHON=${{FRAMEYAP_PYTHON:-{python_default}}}\n'
            + f'MODEL=${{FRAMEYAP_MODEL:-{model_default}}}\n'
            + 'if [ "$#" -eq 0 ]; then set -- --run; fi\n'
            + 'case "$1" in\n'
            + '  --run)\n'
            + '    if [ -n "${GAMESCOPE_SOCKET:-}" ]; then set -- "$@" --socket "$GAMESCOPE_SOCKET"; fi\n'
            + f"    shift; exec {q(base / 'bin/frameyap')} --run {args} --python \"$PYTHON\" --model \"$MODEL\" \"$@\";;\n"
            + f"  --check-overlay|--check-controls) mode=$1; shift; exec {q(base / 'bin/frameyap')} \"$mode\" --assets {q(base / 'assets')}{check_font} \"$@\";;\n"
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
                     (desired_launcher(root, pinned_font=True), desired_launcher(root, legacy=True)))
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


def do_install(args, root, launcher):
    version = check_version(args.version)
    versions = root / "versions"
    owned_dir(versions)
    current = selected(root, "current")
    selected(root, "previous")
    # Refuse foreign wrappers/launcher directories before installing a new version.
    owned_dir(launcher.parent)
    check_wrappers(root, launcher)
    if args.archive:
        archive = Path(args.archive).expanduser().resolve(strict=True)
        expected = check_digest(args.sha256)
        do_download = False
    else:
        do_download = True
        archive = None
    with tempfile.TemporaryDirectory(prefix=".download-", dir=root) as td:
        if do_download:
            name = release_name(version)
            url = f"https://github.com/{args.repo}/releases/download/{quote(version)}/{name}"
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
            print("ASR runtime is NOT included. Supply an independently authorized environment with FRAMEYAP_PYTHON or --python; no packages are downloaded.")


def uninstall(root, launcher):
    current = selected(root, "current")
    previous = selected(root, "previous")
    manifest = root / "frameyap.vrmanifest"
    desired = (json.dumps(desired_manifest(launcher), sort_keys=True, indent=2) + "\n").encode()
    check_owned_file(manifest, desired)
    check_owned_file(launcher, desired_launcher(root),
                     (desired_launcher(root, pinned_font=True), desired_launcher(root, legacy=True)))
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
    print("FrameYap removed; config and saved models preserved. OpenVR unregister acknowledgement was required.")


def main(argv=None):
    parser = argparse.ArgumentParser(prog="install.sh", description="User-local FrameYap release installer (no SteamVR actions)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--archive", help="local release archive (requires --sha256 and --version)")
    mode.add_argument("--rollback", action="store_true")
    mode.add_argument("--uninstall", action="store_true")
    parser.add_argument("--sha256")
    parser.add_argument("--version")
    parser.add_argument("--repo", default="baketnk/frame-yap")
    parser.add_argument("--without-model", action="store_true")
    parser.add_argument("--unregistered", action="store_true", help="acknowledge explicit OpenVR removal before uninstall")
    args = parser.parse_args(argv)
    if args.repo != "baketnk/frame-yap":
        parser.error("only the pinned baketnk/frame-yap release repository is supported")
    if args.archive and (not args.sha256 or not args.version):
        parser.error("--archive requires --sha256 and --version")
    if not args.archive and args.sha256:
        parser.error("--sha256 only applies to --archive")
    if not (args.rollback or args.uninstall or args.archive) and not args.version:
        parser.error("--version TAG is required; no moving/latest release")
    if args.uninstall and not args.unregistered:
        parser.error("uninstall requires --unregistered after explicit OpenVR unregister")
    if args.unregistered and not args.uninstall:
        parser.error("--unregistered only applies to --uninstall")
    if args.version:
        check_version(args.version)
    check_host()
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share").expanduser().absolute()
    root = data / "frameyap"
    launcher = Path.home() / ".local/bin/frameyap"
    owned_dir(root)
    lock = root / ".lock"
    if lock.is_symlink():
        fail("refusing symlink lock")
    with lock.open("a+b") as fd:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("FrameYap installer or application is running (.lock held); try again later")
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
        else:
            do_install(args, root, launcher)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError, json.JSONDecodeError) as exc:
        print(f"frameyap installer: {exc}", file=sys.stderr)
        sys.exit(1)
