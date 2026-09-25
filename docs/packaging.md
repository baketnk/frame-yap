# Release packaging and idempotent user-local installer

**No GitHub release is published.** Native-only local artifacts have been installed
and reinstalled on Frame. The installer never pretends an ASR runtime is included when it is not;
see [third-party notes](third-party.md).

## Producer

Default builds stay offline. Producers explicitly provision native dependencies,
build with `FRAMEYAP_NATIVE=ON`, and stage this layout (regular files, no links):

```
bin/frameyap
bin/install.sh                    # managed installer helper when required by layout
lib/*                             # explicitly supplied compatible native libraries
assets/actions.json               # plus binding JSON and backends/redux.json
fonts/font.ttf
python/frameyap/*.py
scripts/model-status.py           # offline CLI verifier; also backend-service.py, fetch-model.py
licenses/THIRD_PARTY_NOTICES.txt
model/*                           # optional pinned public weights + attribution
runtime/bin/python3               # ONLY for an authorized bundled-runtime artifact
```

The stage copies the self-contained installer into `bin/` and CMake installs
`scripts/model-status.py` alongside the backend manifests. The package allowlist
permits `scripts/`; installed `frameyap --list-models` expects the verifier at
`../scripts/model-status.py`. **No actual staged archive has been audited/tested
from a clean account for publication**; exercise the entire producer pipeline
and the installed CLI before treating the payload layout as release-ready.

For the current **external-runtime** package, `scripts/stage-native.py --help`
documents explicit inputs. The old `scripts/stage-native-poc.py` is retained as
a deprecated migration wrapper for that command, not the documented or shipped
staging interface. The stage invokes `cmake --install` on an existing native
build, copies SDL/OpenVR and a font. It does
not build, download, run the app, or copy an ASR runtime. The native
app relies on Frame's system Vulkan loader/driver, Wayland, libxcb, FreeType,
libstdc++ and glibc. Check the actual staged ARM64 binaries' `NEEDED`,
`GLIBC_*`/`GLIBCXX_*` symbol versions and ELF interpreter; then test
on a clean target. No compatible libc floor is yet established.
SDL/OpenVR resolve inside its own `lib/`, not a producer
prefix. ARM64/glibc packaging is not a claim of compatibility with arbitrary Linux.

```sh
python3 scripts/package-release.py --stage /path/to/stage --output /existing/output \
  --version 0.1.202609241627 --arch linux-aarch64 \
  --model-revision fad622f25f303105c20d70e201bcc477c88b620c --external-runtime
```

Use the actual binary's numeric `MAJOR.MINOR.YYYYMMDDHHMM` UTC version
(currently `0.1`), not this illustrative value, for `--version` and filename;
tag a vetted clean tree as `vVERSION`. CMake generates this stamp at
configuration time (`SOURCE_DATE_EPOCH` may supply it);
`-DFRAMEYAP_VERSION=0.1.YYYYMMDDHHMM` can pin it. Development `--version`
may print a *separate* `git HASH` line, with `(uncommitted changes)` only if
dirty: that line is not part of the version, tag or archive name. The installer
rejects a reused tag with different contents; historic local versions may
remain selectable for rollback, not as new releases.

`--external-runtime` refuses a runtime directory and records
`runtime: external-authorized-python` in `release.json`. The installer explicitly
reports that ASR is not supplied. Without that flag, a complete compatible isolated CPU Python runtime is required
in the archive. Staging validation is not an inference test.

The producer refuses overwrites and emits `frameyap-VERSION-linux-aarch64.tar.gz` plus `.sha256` containing
`HASH  FILENAME`. Archive extraction rejects traversal,
links/special files, duplicate members, oversized metadata/payloads and invalid
layout. Checksums detect corruption, not a malicious/compromised publisher;
authenticate release metadata independently. No packaging/installation model fetch.

## Consumer

Bootstrap for a binary archive: Linux ARM64/glibc, Python 3.12+ (installer
bootstrap, **not** the inference runtime), curl for network release downloads,
sha256sum and tar for archive handling. **No compiler, sudo, Steam store AppID
or engine checkout in binary mode.** No minimum glibc floor has been certified;
preflight alone cannot guarantee compatibility. Download/inspect a pinned installer
before running it. Current local artifact route:

```sh
sh install.sh --archive /path/to/frameyap-VERSION-linux-aarch64.tar.gz \
  --sha256 64_HEX_DIGIT_HASH --version VERSION
```

After an actual vetted release exists, use a real numeric version (for example,
`sh install.sh --mode binary --version 0.1.202609241627 --yes`); the installer
will retrieve tag `v0.1.202609241627` and its versioned checksum; no `latest` or moving-branch
lookup. A pipe invocation is supported, never prompts on stdin, and must also
pin a real published tag. **There is no functional public download command yet.**

`--without-model` omits any model files in the selected archive, never deletes
an existing current model on rerun, and records the choice. Same
digest/version/choice is idempotent and repairs missing managed wrappers. Different digest or model choice for the
same version is refused. External model provisioning is always deliberate: the
local installer accepts `sh install.sh --install-model --backend redux --yes`
(optional `--model-dir /absolute/path`) to explicitly download and verify files from the
installed pinned manifest, not an ASR runtime. Inspect the model/size first
with `sh install.sh --install-model --backend redux --print-plan --json`.
`--expected-manifest-sha256 HASH` additionally binds consent to the exact raw
installed `redux.json` bytes: under the model lock a mismatch fails **before**
a model directory is created or any network request. In the locally implemented
Models UI, Install displays source, rounded size, license text, attribution and
the fingerprint; only the second Confirm Install click passes that fingerprint
through the backend helper to the installer. Model installation does not
provision a CPU Python runtime; no in-panel flow has been accepted on Frame.

`sh install.sh --install-runtime --yes` explicitly creates a user-local venv
under `$XDG_DATA_HOME/frameyap/runtimes/` from the invoking `python3` (3.10–3.13),
pip-installs `torch==2.8.0` from PyTorch's CPU index, then `moondream==2.4.0`
(which pins kestrel 0.8.0, kestrel-native 0.1.8, kestrel-kernels 0.7.0) from PyPI
with Torch constrained, binary wheels only and `pip --isolated`. It verifies the
imports and that Torch has no CUDA, then sets `python=` in `paths.conf` (other
lines kept) and removes superseded FrameYap-managed runtimes. A failure removes
only the new venv and leaves `paths.conf` unchanged. `--print-plan --json` shows
the packages, index and size (about 200 MB of wheels, roughly 1–1.5 GB on disk)
without touching anything. The app must be closed (install lock). Transitive
dependencies are not pinned. Validated end to end on x86-64 Python 3.12
(2026-09-25); a clean Frame install is still pending.
In the source tree, `python3 scripts/model-status.py --list-models` or
`--check-model redux --model-dir /absolute/model` hash-checks local files;
the native `frameyap --list-models` / `--check-model` entry points use the
adjacent installed verifier script; verify this in a staged archive before release. Manifest schema,
source, size/hash and attribution live in `assets/backends/redux.json` and are
validated by `python/frameyap/model_files.py`.

For an explicit local **source** install, e.g.:

```sh
sh install.sh --mode source --source /absolute/source --openvr-root /absolute/sdk \
  --openvr-library /absolute/libopenvr_api.so --openvr-license /absolute/openvr/LICENSE \
  --sdl-library /absolute/libSDL3.so.0 --sdl-license /absolute/sdl/LICENSE \
  --version 0.1.202609241627
```

It checks tools and native dependencies, builds/stages/packages in a private
workspace, then installs the result. Unlike binary mode, this requires a C++
compiler, CMake, SDK, SDL3, Wayland/scanner, libxcb, FreeType, Vulkan development
files and producer-supplied
licenses; it still does not install Python ASR packages. `--print-plan` performs
a read-only plan, `--json` gives machine-readable results/errors (model installs
also stream file events), and `--yes` authorizes network downloads. Bare TTY
invocation can guide choices and prints equivalent flags; non-TTY runs require
explicit arguments and never prompt. `--autolaunch`/`--no-autolaunch` are explicit
OpenVR registration choices, off by default; do not pass either during an inert
install if a running SteamVR session must remain untouched.

The installed launcher defaults to `--run` and forwards explicit run flags
(including `--backend ID`, `--model-store /absolute/store` and
`--manifest-dir /absolute/manifests`) to the binary. These overrides select
local metadata/model paths, not a runtime download; the latter two require
absolute paths without dot segments. `--backend` selects for that run unless
changed in the panel; a saved `config.json` backend is otherwise used. For a
native-only package, supply
`FRAMEYAP_PYTHON=/absolute/authorized/python` and `FRAMEYAP_MODEL=/absolute/model`,
or override `--python`/`--model` on an explicit `--run`. For menu launches, create
`$XDG_CONFIG_HOME/frameyap/paths.conf` (default `~/.config/frameyap/paths.conf`):

```text
python=/absolute/path/to/independently-authorized/runtime/bin/python3
model=/absolute/path/to/pinned/local/model
```

The launcher reads these as **literal absolute paths**, not shell code, and does
not follow a symlink to the config file. Environment variables override either
setting for an explicit launch. This `paths.conf` survives upgrade/uninstall;
only `--install-runtime` writes its `python=` line, and no runtime is bundled. No
pip/bootstrap/model download or fallback is invoked by the launcher. Without
paths, the native-only artifact cannot perform voice inference; its default
runtime and model locations do not exist. A new installer accepts only the exact
previous managed launcher bytes for migration; a modified launcher is refused.
The current managed launcher leaves `--font` unset so `config.json` can choose it.

Without any ASR runtime, these checks work directly through the installed launcher:

```sh
~/.local/bin/frameyap --check-input
~/.local/bin/frameyap --check-overlay --head
~/.local/bin/frameyap --check-controls --head
```

These modes cannot record or type. `GAMESCOPE_SOCKET` or `--socket` selects the
input backend; default is `GAMESCOPE_WAYLAND_DISPLAY`, then `gamescope-0`.

## Lifecycle

Install root: `$XDG_DATA_HOME/frameyap` (default `~/.local/share/frameyap`);
launcher: `~/.local/bin/frameyap`; desktop entry:
`$XDG_DATA_HOME/applications/frameyap.desktop` (default
`~/.local/share/applications/frameyap.desktop`). The desktop entry points to the
user-local launcher; the installer never edits Steam's library or registers a
Steam shortcut. Install/upgrade creates or checks `$XDG_CONFIG_HOME/frameyap/config.json`
(default `~/.config/frameyap/config.json`), filling missing properties or repairing
invalid JSON/values while preserving valid customization. Before each repair it
saves an exact-byte `config.json.backup-*` next to the original; valid config
is left untouched. Symlinks and oversized config files are refused, and uninstall
leaves both the config and backups in place. `paths.conf` is not modified. The launcher
passes a stable install-root lock identity and sets `PYTHONDONTWRITEBYTECODE=1`.
Runtime/check/registration modes and installer share an exclusive nonblocking
`.lock`; no upgrade/rollback/uninstall kills a running app or any other process.

Selection of a completed `current` is atomic; `previous` is retained.
`sh install.sh --rollback` switches to the prior validated version. Foreign/modified
wrappers, untracked install files and inconsistent ownership metadata are refused.
Same-user malicious concurrent filesystem mutation is outside the current threat model.

The generated `frameyap.vrmanifest` uses `local.frameyap.overlay`, **not a store
AppID**. Linux ARM requires `binary_path_linux_arm`; both Linux fields are written.
Installation does not initialize OpenVR, register, autostart, record or type.
With SteamVR ready, explicitly register:

```sh
~/.local/bin/frameyap --register "$HOME/.local/share/frameyap/frameyap.vrmanifest"
# Substitute XDG_DATA_HOME if customized. Add --autostart only if deliberately wanted.
```

Registration checks actual OpenVR installed state rather than trusting Add's return
alone. Repeated registration/removal were exercised on Frame, autolaunch off.
Actual menu launch and cold-runtime behavior remain separate acceptance checks.

## SteamVR dashboard launcher check (opt-in on Frame)

Registration creates an **OpenVR app entry**, not a Steam store/library shortcut.
On one Frame, registration succeeded with autolaunch off but **no FrameYap entry
was visible in the first checked dashboard menu**. The user then found FrameYap
under Steam's **Non-Steam** section and reported that selecting it opened the
panel and Quit closed it. The VR server logged a `--run` overlay connection and
exit; no owned process remained. This establishes a basic menu-driven launch on
that device, **not** that OpenVR registration alone creates a Steam shortcut or
that the new desktop entry was the sole discovery path. The manifest points to
`~/.local/bin/frameyap`, which starts `--run` if launched. A native-only install
without configured model/runtime cannot transcribe; it should show **Unavailable**
(possibly after a brief Warming transition), rather than record or infer.
Only perform this check on an unconfigured native-only installation: verify
that `current/runtime/bin/python3` and `current/model` are absent and no
user-local paths are configured; do not press Record, Type or Type + Enter.
The installer also provides a desktop entry; where Steam's UI supports adding a
non-Steam app, the user may select that entry or browse to the installed launcher.
Shortcut discovery/persistence after a normal restart is not yet verified. Do
not hand-edit Steam's shortcut database.

1. With SteamVR running, register using the command above (do not pass
   `--autostart`). Leave any other apps and sessions alone.
2. In the headset, check Steam's **Non-Steam** section or the **+** application
   picker for FrameYap. If absent, stop and report the menu checked; registration
   alone does not add a Steam library shortcut. If present, select it, check the
   FrameYap panel's status, and use **Quit** to close.
3. Report separately whether the menu entry was visible, the click started an
   app, the panel was visible, and Quit worked. A registration success or CLI
   `--check-overlay` success alone does **not** establish menu-driven launch.

This check does not validate microphone capture, transcription, controller input,
text delivery or cold SteamVR startup. With a configured inference runtime, the
menu launch attempts to load the model; defer that test unless you intend to load the model. If an
environment/configuration unexpectedly supplies a runtime/model, do not perform
this inert launcher check.

Before uninstall, explicitly `frameyap --unregister /absolute/manifest/path` and
verify success, then run `sh install.sh --uninstall --unregistered`. The latter is
an acknowledgement, not a hidden SteamVR edit. Only owned files are removed;
config stays, models move to `saved-models/VERSION`, conflicts/untracked files abort.

Offline tests: `python3 -m unittest discover -s tests -p test_installer.py`.
They use temporary homes/local fixtures. When editing the Python helper,
run `python3 scripts/sync-installer.py`; tests enforce embedded installer parity.
