# Frame Dictation

Standalone, on-device voice typing for Steam Frame.

**Status: project scaffold and design only.** The executable prints help/version;
it does not yet render an overlay, open a microphone, load a model or type text.

Planned path: **OpenVR overlay → local Parakeet Redux CPU inference → Gamescope
Unicode input**. No external application checkout, library, submodule, desktop
inference server or running scene host is required.

**Distribution goal:** one-command installation from GitHub releases, entirely
user-local, with no Steam store AppID. See the [installer design](docs/install-design.md).
There is no working installer or published release yet.

For a read-only check of proposed release prerequisites on a Frame, run
`sh scripts/install-preflight.sh`. It checks Linux AArch64/glibc and bootstrap
tools, reports system Python (3.12+ for a possible source worker), Git and uv.
Git and uv are not required for the planned bundled release. This check downloads
and installs nothing; passing it does **not** mean an install or dictation works.

## Build the scaffold

Requirements: CMake 3.20+ and a C++20 compiler. No third-party packages or downloads.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/frame-dictation --help
```

This is a host-native scaffold build, not an ARM64 deployment or headset test.
Future OpenVR, audio and inference integrations must be explicitly configured;
configuration/build/tests must never install packages or launch SteamVR implicitly.

## Project map

- [Design](docs/design.md): UI, local inference, input backend and acceptance gates.
- [Frame API evidence](docs/evidence/frame-dictation-apis-2026-09-24.md): successful
  mixed-script Unicode test and overlay-client initialization; known limits.
- [Installer design](docs/install-design.md): GitHub install, standalone OpenVR
  identity, packaging, upgrades and uninstall.
- [Provenance](docs/provenance.md): origin of the investigation, model/runtime pins.
- `src/main.cpp`: inert CLI entry point.
- `tests/cli.cmake`: hardware-free scaffold smoke test.
- `scripts/install-preflight.sh`: read-only proposed packaging prerequisite check.
- [Agent guidance](AGENTS.md): project boundaries and safe validation.

First implementation gate: an isolated ARM64 CPU trial of the pinned Redux model.
Then a minimal overlay and explicit insertion into a disposable target. Neither
inference nor an overlay is implemented here yet. No model weights, recordings,
credentials, engine assets or runtime binaries are included.
