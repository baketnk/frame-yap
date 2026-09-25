# Installation goal and current boundary

Goal: a one-command, pinned GitHub release install for Steam Frame without a Steam
store AppID, sudo or end-user compiler. **No public archive or verified clean
install is published. Do not advertise a `curl | sh` command as functional.**
The local installer has binary-archive and explicitly provisioned source-build
modes, machine-readable plans/results and an attended TTY path. The current
native-only artifact does not include or pip-install an ASR runtime; it is not
a one-command voice-typing experience. See [packaging](packaging.md) for exact
flags and [third-party notes](third-party.md).

`scripts/install-preflight.sh` is a read-only Linux ARM64/glibc/bootstrap check;
`--source` adds toolchain/library checks. It does not download, install, register,
or certify model/runtime compatibility or a minimum libc version. Its report of
Python/Git/uv availability is not a runtime guarantee: `install.sh` currently
needs Python 3.12+ **for its bootstrap**, while native-only voice inference needs
a separately provisioned compatible CPU Python environment and pinned weights.

## OpenVR identity

`local.frameyap.overlay` is a string OpenVR application key, **not** a Steam
store AppID. The installer creates a manifest/desktop launcher user-locally but
does not register/launch the app by default. Explicit `frameyap --register
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
  moondream, Kestrel or an interpreter. Launch paths to an independently
  authorized runtime/model can be set in `paths.conf`.
- **Noninteractive**: supply flags and `--yes` for network/model consent.
  `--print-plan` is read-only; `--json` provides structured results/errors and
  progress events for a model download. No prompt reads stdin in a pipe. An
  empty TTY invocation offers a local menu and prints equivalent flags.

Installation is user-local under XDG data/config paths with a managed launcher,
retained rollback, SHA-256/path validation, foreign-file refusal and a lock
shared with the app. No OS package changes, udev rule, root service, Steam store
listing, unrelated application dependency, microphone recording, input injection
or automatic update daemon. Hashes detect accidental/unauthorized alteration
of a downloaded artifact but do not authenticate a compromised publisher;
release metadata needs independent trust. No installer operation silently runs
pip or launches inference. Native-only archives support overlay checks but need
an externally provisioned runtime/weights before voice typing. Selecting an
uninstalled backend does not authorize a download or supply its inference engine.

## Gate before publishing the goal as fulfilled

Vet the exact release closure, ARM64 symbol versions/loader
and a compatible CPU Python environment; establish a tested
libc/runtime floor, then publish and authenticate a checksummed archive from a
clean tag. On a clean supported Frame, install without a compiler/sudo/store ID,
load Redux, type into a disposable owned target and validate rollback/uninstall,
foreign-file failures, autolaunch off, and no unintended session changes. Separately
validate microphone → review → delivered input and headset comfort. Offline
installer fixture tests and a local native build are not those acceptance gates.
