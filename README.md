# FrameYap

Standalone, on-device voice typing POC for Steam Frame. **MIT licensed.**

Implemented: native OpenVR overlay, remappable controller actions, bounded SDL3 capture,
persistent local Parakeet Redux worker, preview/explicit insertion through Gamescope,
and an idempotent user-local installer. No Steam store AppID, sudo, desktop ASR
server, cloud fallback or unrelated application dependency.

**Status:** native ARM64 build, CPU inference on a public clip, overlay visibility,
Gamescope discovery and native-only installation have been exercised on Frame.
Live microphone → reviewed text → real target delivery is **not yet accepted**.

**Runtime licensing remains unresolved.** Redux weights are CC-BY-4.0, but the
observed Kestrel kernel runtime requires separate permission. We are checking with
the vendor. Current native-only packages do **not** bundle or download that
runtime; an independently authorized Python environment must be supplied.
See [third-party notes](docs/third-party.md). No GitHub release is published yet.

## Controls

- **Right X (default Frame binding):** hold to record; release to
  transcribe. This PTT action is remappable through SteamVR bindings; delivery
  in the dashboard still requires an on-device test.
- **Left grip (when active):** double-tap to explicitly send Enter. Never inferred from speech.
- **Overlay:** Record/Stop, Cancel, paginated preview, Insert, Enter and Quit.
  Review and settings share one Inconsolata/neon-framed surface. World-space by
  default; settings offer left wrist, right wrist and head mounting, plus recenter.
  Dashboard lasers provide clickable controls without forcing global laser mode.
- **Theme and controls:** optional `$XDG_CONFIG_HOME/frameyap/config.json` selects
  panel colors, a font path and Frame controller button mappings; missing fonts
  fall back to bundled Inconsolata. The installer creates/checks this file and
  backs it up before repairs. See [overlay configuration](docs/overlay.md#user-theme-and-controller-configuration).
- **Review-first:** focus your destination, then press Insert. No automatic insertion
  or submission. Maximum clip 20 seconds; accidental taps under 200 ms are discarded.
- Other apps may still hear/transmit your voice. FrameYap does not mute them.

Physical gesture timing, global bindings during games, mic capture, target-app
compatibility and headset comfort still need coordinated validation. Successful
API initialization is not delivered input or human acceptance.

## Build and test offline

CMake 3.20+, C++20 compiler. Python 3.10+ runs the additional hardware-free tests.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/frameyap --help
```

Default build has no hardware backends. It never downloads packages/models or
initializes SteamVR, a microphone or input injection. Native dependencies and
explicit launch/check commands are documented in [the POC guide](docs/poc.md).
`--version` uses an ISO-like UTC build timestamp (with Git hash when available),
not a numbered release; use that same tag when packaging the binary.

## Installation

The real installer accepts a versioned, checksummed prebuilt ARM64 archive:

```sh
sh install.sh --archive /path/to/frameyap-VERSION-linux-aarch64.tar.gz \
  --sha256 ARCHIVE_SHA256 --version VERSION
```

This is the **local-artifact command shape**, not an available public download.
Installation is user-local, retains rollback, refuses active-app upgrades and
foreign files, and does not launch or register automatically. Registration uses
OpenVR application key `local.frameyap.overlay`, not a Steam store AppID.
Autolaunch is opt-in. An installed desktop entry can be selected manually as a
non-Steam shortcut. A basic launch from Steam's Non-Steam section opened the
panel on one Frame; registration alone did not show an entry in the first checked
dashboard menu. See [packaging and lifecycle](docs/packaging.md).
For a native-only install, menu-driven inference needs a separately authorized
Python runtime and pinned model. Configure their absolute paths in
`~/.config/frameyap/paths.conf` as described in the packaging guide; they are
never fetched or bundled implicitly.

A pinned GitHub one-command route is implemented in `install.sh --version TAG`,
but **do not advertise or run it as a working public installation until a vetted
release exists**. Native-only artifacts support the overlay/checks without an
end-user compiler; full bundled-ASR distribution awaits runtime permission.

## Project map

- [POC guide](docs/poc.md): implemented boundaries, build, controls, explicit tests.
- [Design](docs/design.md): full target design; some features remain proposed.
- [Installer design](docs/install-design.md) and [packaging](docs/packaging.md).
- [Current POC observations](docs/evidence/poc-cpu-overlay-2026-09-24.md): measured
  CPU behavior and native installation checks, with acceptance limits.
- [Earlier API evidence](docs/evidence/frameyap-apis-2026-09-24.md) and
  [provenance](docs/provenance.md): historical investigation, not live authority.
- [Overlay](docs/overlay.md), [worker protocol](docs/worker.md),
  [dependency/license inventory](docs/third-party.md).

No recordings, transcripts, private logs, model weights or runtime binaries are
committed. The worker boundary is intentionally small for forks experimenting
with other models/APIs; the default remains local-only Redux.
