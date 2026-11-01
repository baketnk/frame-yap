# Installation goal and current boundary

Goal: a one-command, pinned GitHub release install for Steam Frame without a Steam
store AppID, sudo or end-user compiler. Release v0.1.202609251524 publishes an
ARM64 archive, its `.sha256` and `install.sh` on GitHub, built on a Frame with
`scripts/build-release.sh`; `curl …/releases/latest/download/install.sh | sh`
runs the attended flow. A clean-install check on Frame is the remaining gate.
The local installer has binary-archive and explicitly provisioned source-build
modes, machine-readable plans/results and an attended TTY path. The native-only
artifact does not include an ASR runtime; a separate explicit
`--install-runtime --yes` step uses shared user-level uv and uv-managed CPython
3.12 to install the pinned CPU speech packages into a dedicated FrameYap venv.
App, model and runtime installs remain separate consented steps. See
[packaging](packaging.md) for exact flags and [third-party notes](third-party.md).

`scripts/install-preflight.sh` is a read-only Linux ARM64/glibc/bootstrap check;
`--source` adds toolchain/library checks. It does not download, install, register,
or certify model/runtime compatibility or a minimum libc version. Its report of
Python/Git/uv availability is not a runtime guarantee: the installer and updater
need system Python 3.12+ **for their bootstrap**, while voice inference uses a
separately provisioned CPU environment and pinned weights. uv is optional at
preflight: explicit runtime setup reuses it from `PATH` or the normal user bin,
or installs it if missing. Preflight itself never bootstraps uv or Python.

## OpenVR identity

`local.frameyap.overlay` is a string OpenVR application key, **not** a Steam
store AppID. The installer creates a manifest and a desktop entry user-locally; the
desktop entry (read by KDE's menu) is the intended launch path, and the app
is not registered with SteamVR or launched by default. Explicit `frameyap --register
/absolute/manifest/path` uses the OpenVR registration API; registration alone
did not reveal a launcher in the first checked dashboard menu. On one Frame the
user opened the panel from the **Non-Steam** section and quit; shortcut discovery
and persistence after a normal restart are still unverified. Optional
`--autolaunch`/`--no-autolaunch` on the installer explicitly request OpenVR
registration/autolaunch choices; no SteamVR settings or sessions are changed
without that request. Unregister explicitly before uninstall. SteamVR must
already be available for registration; never start/restart it for installation.

## Installation workflows

- **Binary mode** (no compiler): verify a locally supplied ARM64 archive digest,
  or, after a vetted release actually exists, explicitly approve retrieval of
  a pinned `v0.1.YYYYMMDDHHMM` GitHub tag and checksum. No moving `latest` tag.
  `--mode binary --archive FILE --sha256 HASH --version 0.1.YYYYMMDDHHMM`
  selects the local-artifact path.
- **Source mode**: requires explicit local source, SDK, SDL/OpenVR libraries and
  their notices, CMake/C++20, native build dependencies and a version. It builds,
  stages, packages and continues through local installation; it does **not**
  provision ASR packages. See
  [packaging](packaging.md) for all flags.
- **Model**: only an explicit `--install-model --backend redux --yes` fetches
  pinned public files for the *already installed* backend. Inspect the read-only
  `--print-plan --json` first; it includes model size, attribution and license
  metadata from `assets/backends/redux.json`. `--expected-manifest-sha256 HASH`
  binds consent to the exact installed manifest bytes and fails before model
  directory creation/network if they changed. The local in-panel chooser shows
  source, size, license text, attribution and manifest digest, then requires a
  second **Confirm Install** click; the installed/native UI route still needs
  clean-target and headset acceptance. `--without-model` permits an
  archive install without bundled model files. Neither operation installs Torch,
  moondream or Kestrel. Launch paths to an independently authorized runtime/model
  can be set in `paths.conf`.
- **Runtime**: only explicit `--install-runtime --yes` (or attended runtime
  consent) provisions speech packages. If uv is missing, setup downloads/runs
  <https://astral.sh/uv/install.sh> for standard shared user-level uv; the script
  may modify shell profiles unless `UV_NO_MODIFY_PATH=1` is set. `uv venv` uses
  managed CPython 3.12 under `$XDG_DATA_HOME/frameyap/runtimes/cpu-TIMESTAMP`;
  `uv pip install` selects `torch==2.8.0` from the CPU index, then
  `moondream==2.4.0` from PyPI with Torch constrained, binary wheels only and no
  pip fallback. The prompt/read-only plan disclose additional uv/Python downloads
  alongside about 200 MB of wheels and a roughly 1–1.5 GB speech environment;
  those estimates are not a total for the bootstrap/shared storage. uv and exact
  Python builds are not app-pinned; speech top-level pins are unchanged and
  other transitives still float. See [runtime details](packaging.md#speech-runtime)
  for trust, storage overrides, verification and cleanup behavior. **Real
  ARM64/Frame verification of this uv path is pending**; historical pip-based
  installation evidence does not validate it.
- **Noninteractive**: supply flags and `--yes` for network/model/runtime consent.
  `--print-plan` is read-only; `--json` provides structured results/errors and
  progress events for a model download. No prompt reads answers from a pipe.
  A bare terminal invocation, including `curl … | sh`, prompts on the real
  terminal (`/dev/tty`), asks y/n separately for the model and runtime
  downloads, and prints the equivalent flag commands.

Installation is user-local under XDG data/config paths with a managed launcher,
retained rollback, SHA-256/path validation, foreign-file refusal and a lock
shared with the app. No OS package changes, udev rule, root service, Steam store
listing, unrelated application dependency, microphone recording, input injection
or automatic update daemon. Hashes detect accidental/unauthorized alteration
of a downloaded artifact but do not authenticate a compromised publisher;
release metadata needs independent trust. No installer operation silently
provisions speech dependencies or launches inference. Launch and native updates
never bootstrap uv/Python or install speech packages; existing working speech
environments are untouched by native updates. An older install can update its
native release first, then run the **new**
`current/bin/install.sh --install-runtime --yes` if repair is needed. Shared uv,
Python storage and package caches are retained on failure and uninstall.
Native-only archives support overlay checks but need a provisioned runtime/model
before voice typing. Selecting an uninstalled backend does not authorize a
download or supply its inference engine.

## Gate before publishing the goal as fulfilled

Vet the exact release closure, ARM64 symbol versions/loader
and a compatible CPU Python environment; establish a tested
libc/runtime floor, then publish and authenticate a checksummed archive from a
clean tag. On a clean supported Frame, install without a compiler/sudo/store ID,
load Redux, type into a disposable owned target and validate rollback/uninstall,
foreign-file failures, autolaunch off, and no unintended session changes. Separately
validate microphone → review → delivered input and headset comfort. Offline
installer fixture tests and a local native build are not those acceptance gates.
