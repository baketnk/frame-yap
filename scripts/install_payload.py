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


def desired_launcher(root):
    import shlex
    q = lambda path: shlex.quote(str(path))
    font = root / "current/fonts/font.ttf"
    base = root / "current"
    flags = ["--assets", base / "assets", "--font", font,
             "--worker", base / "python/frameyap/worker.py"]
    args = " ".join(q(item) for item in flags)
    return ("#!/bin/sh\n" + MARKER + 'export PYTHONDONTWRITEBYTECODE=1\n'
            + f'export FRAMEYAP_INSTALL_ROOT={q(root)}\n'
            + f'export LD_LIBRARY_PATH={q(base / "lib")}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}\n'
            + f'PYTHON=${{FRAMEYAP_PYTHON:-{q(base / "runtime/bin/python3")}}}\n'
            + f'MODEL=${{FRAMEYAP_MODEL:-{q(base / "model")}}}\n'
            + 'if [ "$#" -eq 0 ]; then set -- --run; fi\n'
            + 'case "$1" in\n'
            + '  --run)\n'
            + '    if [ -n "${GAMESCOPE_SOCKET:-}" ]; then set -- "$@" --socket "$GAMESCOPE_SOCKET"; fi\n'
            + f"    shift; exec {q(base / 'bin/frameyap')} --run {args} --python \"$PYTHON\" --model \"$MODEL\" \"$@\";;\n"
            + f"  --check-overlay|--check-controls) mode=$1; shift; exec {q(base / 'bin/frameyap')} \"$mode\" --assets {q(base / 'assets')} --font {q(font)} \"$@\";;\n"
            + f"  *) exec {q(base / 'bin/frameyap')} \"$@\";;\n"
            + 'esac\n').encode()


def desired_manifest(launcher):
    return {"source": "builtin", "applications": [{"app_key": KEY, "launch_type": "binary",
            "binary_path_linux": str(launcher), "binary_path_linux_arm": str(launcher),
            "is_dashboard_overlay": True,
            "strings": {"en_us": {"name": "FrameYap"}}}]}


def check_owned_file(path, expected):
    if path.is_symlink():
        fail(f"refusing foreign symlink: {path}")
    if path.exists():
        if not path.is_file():
            fail(f"refusing foreign path: {path}")
        data = path.read_bytes()
        if data != expected:
            fail(f"refusing to replace modified/foreign file: {path}")


def check_wrappers(root, launcher):
    manifest = root / "frameyap.vrmanifest"
    desired = (json.dumps(desired_manifest(launcher), sort_keys=True, indent=2) + "\n").encode()
    check_owned_file(launcher, desired_launcher(root))
    check_owned_file(manifest, desired)
    return manifest, desired


def install_files(root, installed, launcher):
    owned_dir(launcher.parent)
    manifest, desired = check_wrappers(root, launcher)
    atomic_write(launcher, desired_launcher(root), 0o755)
    atomic_write(manifest, desired)


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
    check_owned_file(launcher, desired_launcher(root))
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
