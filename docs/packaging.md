# Release packaging and idempotent user-local installer

**No GitHub release is published.** Native-only local artifacts have been installed
and reinstalled on Frame. The end-to-end bundled-ASR release remains blocked on
runtime permission; see [third-party notes](third-party.md). The installer never
pretends the proprietary runtime is included when it is not.

## Producer

Default builds stay offline. Producers explicitly provision native dependencies,
build with `FRAMEYAP_NATIVE=ON`, and stage this layout (regular files, no links):

```
bin/frameyap
lib/*                             # compatible bundled native libraries
assets/actions.json               # and adjacent controller binding JSON
fonts/font.ttf
python/frameyap/*.py
licenses/THIRD_PARTY_NOTICES.txt
model/*                           # optional pinned public weights + attribution
runtime/bin/python3               # ONLY for an authorized bundled-runtime artifact
```

For the current **external-runtime** POC, `scripts/stage-native-poc.py --help`
documents explicit inputs. It invokes `cmake --install` on an existing native build,
copies SDL/OpenVR and an explicitly licensed font, and retains notices. It does
not build, download, run the app, or copy a proprietary ASR runtime. The native
POC relies on Frame's system Wayland, FreeType, libstdc++ and glibc; audit `ldd`
on the installed binary. SDL/OpenVR resolve inside its own `lib/`, not a producer
prefix. ARM64/glibc packaging is not a claim of compatibility with arbitrary Linux.

```sh
python3 scripts/package-release.py --stage /path/to/stage --output /existing/output \
  --version v0.1.0-poc --arch linux-aarch64 \
  --model-revision fad622f25f303105c20d70e201bcc477c88b620c --external-runtime
```

`--external-runtime` refuses a runtime directory and records
`runtime: external-authorized-python` in `release.json`. The installer explicitly
reports that ASR is not supplied. Without that flag, a complete independently
licensed, compatible isolated CPU Python runtime is required. **Do not use that
bundled route for Kestrel without permission covering redistribution.** Staging
validation is not a license grant or an inference test.

The producer refuses overwrites and emits `frameyap-VERSION-linux-aarch64.tar.gz`
plus `.sha256` containing `HASH  FILENAME`. Archive extraction rejects traversal,
links/special files, duplicate members, oversized metadata/payloads and invalid
layout. Checksums detect corruption, not a malicious/compromised publisher;
authenticate release metadata independently. No packaging/installation model fetch.

## Consumer

Bootstrap: Linux ARM64/glibc, Python 3.12+, curl, sha256sum and tar. **No compiler,
sudo, Steam store AppID or engine checkout.** Download/inspect a pinned installer
before running it. Current local artifact route:

```sh
sh install.sh --archive /path/to/frameyap-VERSION-linux-aarch64.tar.gz \
  --sha256 64_HEX_DIGIT_HASH --version VERSION
```

After an actual vetted release exists, `sh install.sh --version TAG` retrieves
that GitHub release and its versioned checksum; no `latest` or moving-branch
lookup. A pipe invocation is supported, never prompts on stdin, and must also
pin a real published tag. **There is no functional public download command yet.**

`--without-model` omits bundled model files from staging, never deletes a current
model on rerun, and records the choice. Same digest/version/choice is idempotent
and repairs missing managed wrappers. Different digest or model choice for the
same version is refused. External model provisioning is always deliberate.

The installed launcher defaults to `--run`. For a native-only package, supply
`FRAMEYAP_PYTHON=/absolute/authorized/python` and `FRAMEYAP_MODEL=/absolute/model`,
or override `--python`/`--model` on an explicit `--run`. No pip/bootstrap/fallback
is invoked by the launcher. Missing runtime means visible failure, not recording.
Environment configuration is not automatically persisted for SteamVR autolaunch.

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
Steam shortcut. Existing config is untouched. The launcher
passes a stable install-root lock identity and sets `PYTHONDONTWRITEBYTECODE=1`.
Runtime/check/registration modes and installer share an exclusive nonblocking
`.lock`; no upgrade/rollback/uninstall kills a running app or any other process.

Selection of a completed `current` is atomic; `previous` is retained.
`sh install.sh --rollback` switches to the prior validated version. Foreign/modified
wrappers, untracked install files and inconsistent ownership metadata are refused.
Same-user malicious concurrent filesystem mutation is outside the POC threat model.

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
without a model or authorized runtime should show an **Unavailable** overlay
rather than record or run inference. Only perform this check on that native-only
installation: verify that `current/runtime/bin/python3` and `current/model` are
absent first, and do not press Record, Insert or Enter.
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
menu launch attempts to load the model; defer that test until runtime licensing is
resolved. If an environment/configuration unexpectedly supplies a runtime/model,
do not perform this check.

Before uninstall, explicitly `frameyap --unregister /absolute/manifest/path` and
verify success, then run `sh install.sh --uninstall --unregistered`. The latter is
an acknowledgement, not a hidden SteamVR edit. Only owned files are removed;
config stays, models move to `saved-models/VERSION`, conflicts/untracked files abort.

Offline tests: `python3 -m unittest discover -s tests -p test_installer.py`.
They use temporary homes/local fixtures. Dated live-device observations are in the
[POC record](evidence/poc-cpu-overlay-2026-09-24.md). When editing the Python helper,
run `python3 scripts/sync-installer.py`; tests enforce embedded installer parity.
