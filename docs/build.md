# Build, scope and validation

This is a standalone native application, not a plugin. Default builds/tests never
initialize OpenVR, open a microphone, run ASR, download files or inject input.

## Implemented

- Opt-in OpenVR RGBA overlay with world-space default and selectable wrist/head
  mounts, status, recording timer, paginated UTF-8 preview and explicit controls.
  A saved, default-off Lasers anytime setting requests system-wide laser mouse
  mode while FrameYap is visible; it may affect games, and is not an input override.
- Remappable SteamVR actions. The default Steam Frame binding maps right X
  (hold to record, release to transcribe) to the existing PTT action using the
  observed `frame_controller` profile. Right B cancels, A requests Type (text +
  space), and Y opens/cycles Quick phrases. Type + Enter is the explicit overlay
  control or left-grip double-tap (Enter alone if no preview/phrase). The Bindings button
  requests SteamVR's remapping editor directly. Grip bindings remain, but both grip
  actions were inactive in the observed dashboard check; do not rely on them.
  If left grip becomes active, two short taps request explicit Enter.
  For grip gestures, first squeeze <=250 ms; second squeeze begins <=350 ms
  after first release.
  Activity/tracking loss cancels a held recording and requires neutral rearm.
- SDL3 default recording device, mono float32 conversion at 16 kHz, 200 ms minimum,
  20 second maximum. In the native app the microphone stream opens after model
  warm-up, stays running between utterances, and discards idle samples; PTT does
  not open/pause/close the device **by default**. Settings → Close mic when idle
  (`"close_mic_when_idle": true`, default **false**) closes it between clips and
  reopens on PTT; repeated transitions caused an audio spike on Frame and may
  add latency or clip the first syllable. Quit, worker restart, device failure
  or capture failure also closes it. Keeping it open does not promise glitch-free
  playback on every audio stack. Other apps may still
  transmit your voice: this app does **not** mute VRChat or any other app.
- Local Models chooser and bounded manifest-driven dispatcher: selecting a listed
  backend restarts the worker and invalidates pending clip/review/focus authority;
  a missing or invalid model disables recording. A separate Install then Confirm
  Install exposes pinned source/size/license/attribution and exact manifest-byte
  SHA-256 before installer handoff. These are local implementations, not a tested
  installed UI or an approved additional inference engine. Only Redux is supplied.
- Persistent local Redux worker, correlated bounded pipes, private tmpfs clips,
  cancellation/reaping and deadlines; exact pinned model SHA-256 verification.
  Model imports are lazy and loading is offline. A correlated bad transcript/
  `E` reply is a request-level failure: Record can retry without dropping the
  ready model. Normal repeats and microphone failures retain the loaded model;
  microphone device failure releases the stream for explicit retry. A cancelled
  in-flight request or broken worker protocol may require reloading. No
  cloud/desktop fallback.
- Gamescope IME v2 generated bindings, per-action short-lived lease, unavailable
  handling, UTF-8/control validation and explicit Type + Enter action.
  Type ensures a trailing space without doubling an existing one. Type + Enter
  first types pending review, releases the text lease, then acquires a fresh
  lease for Enter. Failed/uncertain text never proceeds to Enter; failed
  Enter acquisition never replays text. If a validated transcript fills the
  4096-byte bound and lacks a trailing space, Type preserves all its bytes and
  queues it **without** the usual space; it does not signal a separate error.
  Destination consumption and repeated-delivery behavior remain unaccepted.
- User-local installer with checked binary-archive or explicitly provisioned
  source-build mode, safe extraction, atomic current-version selection, retained
  rollback, runtime/install lock, foreign-file refusal and explicit
  unregister-before-uninstall acknowledgement. Source mode needs a local
  compiler, SDK, libraries and license inputs; no runtime is pip-installed.

## Deliberately not claimed

**Review-first by default.** An opt-in Auto insert setting now watches the
Xwayland active window, Gamescope focused-window property and exact keyboard
focus from PTT start to IME lease acquisition. Loss/regain, disagreement,
unavailable focus or a held keyboard key falls back to manual review. No
automatic Enter or inferred speech commands. This focus guard has offline
tests but live microphone → automatic insertion is **not yet accepted** on Frame;
it cannot identify arbitrary native Wayland targets. There remains a race
between the final focus check and global input processing. Text delivery is
reported as **input queued**, not application consumption or message delivery.
A request is consumed once even if transport completion is uncertain; no retries.
Unavailable IME acquisition leaves the preview intact.

No VAD, always-listening mode, desktop transcription service, keyboard emulation
fallback, streaming-PC bridge, or automatic Enter. No general undo. Grip bindings
are not guaranteed globally active in every scene/dashboard state, and the app
never enables SteamVR's experimental overlay overrides on your behalf.

This is one compact panel, not a separate miniature status chip. Front-prefix
loss on repeated submissions (P1) is under separate investigation, **not fixed**
by the chooser/worker documentation or any speculative delivery change. Font
coverage/complex shaping, ergonomics, compositor cost, thermal/battery impact
and target application compatibility require further headset work.

## Inference runtime

Redux weights at `fad622f25f303105c20d70e201bcc477c88b620c` are CC-BY-4.0. Pinned
file sizes/hashes and attribution live in `assets/backends/redux.json`; offline
`--list-models`/`--check-model redux --model-dir /absolute/model` (or
`scripts/model-status.py`) verify without inference or downloads. Manifest
schema/verification are in `python/frameyap/model_files.py`. The local generic
dispatcher resolves a manifest's in-release Python/executable launcher and
checks request/reply correlation; a new manifest still needs its own licensed,
compatible offline runtime and independent tests. Native `--run` flags `--backend ID`, `--model-store /absolute/store` and
`--manifest-dir /absolute/manifests` are wired through
the installed launcher as an explicit override, not a provisioning command.
Inference uses the `moondream` Python package and Kestrel runtime, separately
provisioned in your own environment; builds/tests/installer do not pip-install them. See
[third-party notes](third-party.md).

## Developer native build

Requirements: Linux, CMake/C++20, SDL3 >=3.2, Wayland client + scanner, FreeType,
Vulkan headers/loader (plus the system GPU driver at runtime),
and a deliberately provisioned standalone OpenVR **v2.15.6** SDK. No CMake fetches.

```sh
cmake -S . -B build-native -DFRAMEYAP_NATIVE=ON \
  -DOPENVR_ROOT=/absolute/path/to/openvr
cmake --build build-native -j2
ctest --test-dir build-native --output-on-failure
```

On ARM64, select the SDK's `lib/linuxarm64/libopenvr_api.so` explicitly with
`-DOPENVR_LIBRARY=...` if necessary. Do not use an x86 library or another
application's build/runtime. Installable binary has `$ORIGIN/../lib` RUNPATH;
producer must audit and bundle its compatible dependency closure.

The device development trial built SDL **3.2.16** under a project-owned user
prefix with audio enabled and video/render/GPU/joystick/haptic/sensor/camera/tests
examples disabled (`SDL_UNIX_CONSOLE_BUILD=ON`). No OS package modifications.
That is a producer workflow, not an end-user compiler requirement.

## Explicit hardware checks (not CTest)

```sh
./build-native/frameyap --check-input --socket gamescope-0
./build-native/frameyap --check-overlay --assets "$PWD/assets" --mount world
./build-native/frameyap --check-controls --assets "$PWD/assets" --mount world
```

The first acquires/releases an IME without text/actions. The second displays a
five-second inert panel. The third displays a 30-second diagnostic panel and
reports pointer actions, SteamVR grip and PTT action activity, tracking and
read-only legacy grip state, **without microphone or input injection**. Mount
clicks in check modes do not save a preference. A SteamVR error code or inactive
action means the gesture cannot be accepted; raw grip reads do not authorize a
fallback binding.
Action handles alone are not proof that gestures were delivered. Checks must be explicitly launched
while the user expects the panel. Normal CLI/help/version remain inert.

The ARM64 OpenVR loader's default `/data/work/openvrpaths.vrpath` failed on the
observed device. FrameYap respects `VR_PATHREG_OVERRIDE`; when absent, it selects
an existing standard `$XDG_CONFIG_HOME/openvr/openvrpaths.vrpath` (or
`~/.config/openvr/openvrpaths.vrpath`) before initializing OpenVR. No Steam files
are edited and no runtime/session restart is performed.

## Deliberate launch

Explicit setup downloads only the pinned, openly licensed model:

```sh
python3 scripts/fetch-model.py --destination "$HOME/.local/share/frameyap-model"
```

`fetch-model.py` is idempotent for matching hashes, rejects existing mismatched
files, and is never invoked by build/tests or first utterance.

Provide your own CPU Python environment (moondream 2.4.0,
kestrel 0.8.0) and local weights:

```sh
./build-native/frameyap --run --assets "$PWD/assets" --font /path/font.ttf \
  --python /path/to/runtime/bin/python3 --worker "$PWD/python/frameyap/worker.py" \
  --model "$HOME/.local/share/frameyap-model" --threads 2 --socket gamescope-0 --head
```

Use a disposable text destination first. `--run` loads the model but does not
record until an explicit recording control. Click Type only after focusing your
intended text field. Quit or SIGINT/SIGTERM closes capture, invalidates delivery
and terminates only the owned worker. Installer upgrades refuse an active app.

The `Controller` receives injected `ControllerAudio`, `ControllerWorker`,
`ControllerFocus` and delivery factory interfaces; hardware-free fakes can test
state, retry, authorization and mic lifetime without initializing OpenVR or
recording speech. The worker API is intentionally small: a fork can replace the
worker implementation or add its own model/API integration without changing
overlay and delivery code.
The default product remains local-only Redux; extending a fork does not authorize
sending existing users' audio to a service.

## Benchmarks

`scripts/benchmark-worker.py` accepts a supplied nonprivate PCM16/16 kHz/mono WAV;
no recording, device input or automatic download. `--show-text` is a separate
explicit disclosure of that fixture's transcript. Use only with an authorized
runtime. Benchmark numbers from earlier trials are not headset/performance acceptance.
