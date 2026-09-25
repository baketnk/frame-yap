# Installation and distribution goal

**User goal:** install from GitHub with a `curl … | bash`-style command, without a
Steam store AppID. An idempotent archive installer and native-only local artifacts
are now implemented/tested; no public release is published. A bundled-ASR
experience is not yet offered. This document retains the target
design; see [current packaging](packaging.md) and [third-party notes](third-party.md).
Planned installer work (binary-or-source choice, flag-driven operation) is in
[TODO.md](../TODO.md).

The read-only `scripts/install-preflight.sh` checks whether a host appears suitable
for the **proposed** Linux ARM64 glibc package format and has the expected basic
bootstrap utilities. It reports system Python, Git and uv, but none is required
for the intended bundled release. A read-only check on one Frame observed
Python 3.12.3 and Git, but not uv; availability may change. This script
is not an installer or a model/runtime compatibility test; it has no downloads,
registration, SteamVR initialization or persistent changes. No glibc minimum can
be certified until release artifacts are chosen and tested.

## Non-Steam overlay identity

OpenVR overlay applications do not require a Steam store AppID or Steamworks.
The native executable initializes as `VRApplication_Overlay`. For discoverability
and optional autolaunch, register an OpenVR application manifest with a stable,
project-owned **string application key** (proposed: `local.frameyap.overlay`).
That key is not a numeric Steam AppID. No purchase/store listing or non-Steam Steam
library shortcut should be necessary for the normal route.

The manifest identifies the installed executable; the installer/registration
helper should use `IVRApplications::AddApplicationManifest` and the corresponding
remove operation, not hand-edit Steam's internal JSON. Autolaunch uses the OpenVR
application setting only when explicitly requested. Validate the exact manifest,
launch behaviour, registration persistence and uninstall on native Frame before
claiming this route works end to end. Existing probes established overlay client
initialization, not manifest installation.

SteamVR/OpenVR must already be installed and usable. If registration needs a
running runtime, defer it to the first explicit launch rather than starting or
restarting SteamVR behind the user's back.

## Intended user experience

1. Run one documented command from the eventual GitHub repository/release.
2. Installer identifies native Linux ARM64 Frame, resolves a pinned release and
   explains/downloads the application, compatible CPU runtime and pinned model.
3. User-local installation provides a simple `frameyap` launcher, desktop
   entry where supported, and an OpenVR manifest. No compiler, engine checkout,
   Python dependency troubleshooting or separate ASR server for ordinary users.
4. The user explicitly launches/enables dictation. No installation-time microphone
   recording, input injection, inference benchmark or overlay takeover.

Illustrative command shape only; `OWNER`, `REPO` and `VERSION` are placeholders:

```sh
curl --fail --silent --show-error --location \
  https://raw.githubusercontent.com/OWNER/REPO/VERSION/install.sh | bash
```

Also document a download-inspect-run path for users who do not want to pipe remote
code into a shell. Pin a release/tag instead of executing a moving branch by default.
Offer explicit version selection and noninteractive flags; do not read interactive
confirmation from stdin while the installer itself is arriving through that pipe.

## Packaging boundary

- Prebuilt ARM64 executable plus a known-compatible, isolated CPU inference runtime;
  no external application libraries/assets and no system Python modification.
- Model fetched during explicit installation/setup, with pinned revision/hash and
  attribution. A documented `--without-model` option can defer the large download.
  No surprise first-utterance downloads. Runtime components are installed from
  their own package index rather than redistributed in our tarball.
- Install under `$XDG_DATA_HOME/frameyap` (default `~/.local/share/...`),
  configuration under `$XDG_CONFIG_HOME/frameyap`, optional launcher in
  `~/.local/bin`; transient audio stays in a private `$XDG_RUNTIME_DIR` directory.
- No sudo, OS read-only-root changes, package-manager installs, udev changes,
  `/dev/uinput` permission changes or modifications to unrelated launchers.
- No Steam store AppID, Steamworks SDK, root service or network ASR dependency.
- Third-party notices for everything we redistribute must be included with a release.

## Installer lifecycle and safety

- Detect architecture, libc and prerequisites first. Unsupported hosts fail with a
  clear explanation; never install an x86 payload silently on ARM64.
- Download to a staging directory, check versioned SHA-256 manifests and archive
  paths, then atomically select the completed version. HTTPS/checksums alone do not
  authenticate a compromised publisher; use signed release metadata if provided.
- Keep configuration across upgrades; retain the previous version for rollback.
  Refuse or defer replacement while this application's process is running rather
  than killing arbitrary processes. Never touch SSH or unrelated sessions.
- Autostart is opt-in (`--autostart` or explicit settings); do not enable a systemd
  service or SteamVR autolaunch by default. Do not enable overlay input overrides.
- Uninstall removes only owned launcher, manifest registration and install files;
  model/config deletion is separately explicit. Preserve other SteamVR apps.
- No update daemon initially. A deliberate rerun/update command is sufficient.

## Acceptance before advertising one-command installation

- Clean supported Frame: install without sudo/compiler/engine checkout/store AppID;
  launch overlay, load local Redux and type into an owned disposable target.
- Normal use after installation needs no network connection or desktop ASR host.
- Failed download/hash, unsupported architecture, low disk space and interrupted
  upgrades leave a usable previous install or a cleanly reported failure.
- Reinstall, rollback and uninstall preserve unrelated data and SteamVR entries.
- Noninteractive piped invocation never hangs on stdin; inspection-first path works.
- Autolaunch remains off unless chosen; uninstall removes only our registration.
- Hardware-free installer tests use temporary homes, mocked runtime registration
  and local fixture artifacts. Never exercise a real user's Steam configuration
  in ordinary CI/CTest.
