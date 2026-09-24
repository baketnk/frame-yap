# FrameYap POC: implementation and validation

This is a standalone native application, not a plugin. Default builds/tests never
initialize OpenVR, open a microphone, run ASR, download files or inject input.

## Implemented

- Opt-in OpenVR RGBA overlay with world-space default and selectable wrist/head
  mounts, status, recording timer, paginated UTF-8 preview and explicit controls.
- Remappable SteamVR actions. The default Steam Frame binding maps right X
  (hold to record, release to transcribe) to the existing PTT action using the
  observed `frame_controller` profile. Grip bindings remain, but both grip
  actions were inactive in the observed dashboard check; do not rely on them.
  If left grip becomes active, two short taps request explicit Enter.
  For grip gestures, first squeeze <=250 ms; second squeeze begins <=350 ms
  after first release.
  Activity/tracking loss cancels a held recording and requires neutral rearm.
- SDL3 default recording device, mono float32 conversion at 16 kHz, 200 ms minimum,
  20 second maximum. Microphone is closed outside actual capture. Other apps may
  still transmit your voice: this app does **not** mute VRChat or any other app.
- Persistent local Redux worker, correlated bounded pipes, private tmpfs clips,
  cancellation/reaping and deadlines; exact pinned model SHA-256 verification.
  Model imports are lazy and loading is offline. Normal repeats, request-local
  transcription failures and microphone failures retain the loaded model;
  microphone device streams close outside capture. A cancelled in-flight request
  or broken worker protocol may require reloading. No cloud/desktop fallback.
- Gamescope IME v2 generated bindings, per-action short-lived lease, unavailable
  handling, UTF-8/control validation and explicit separate Submit action for Enter.
- Idempotent user-local release-archive installer: SHA-256, safe extraction,
  atomic current-version selection, retained rollback, runtime/install lock,
  foreign-file refusal and explicit unregister-before-uninstall acknowledgement.

## Deliberately not claimed

**Review-first only.** No automatic insertion or inferred commands. A transcript
must be explicitly inserted into the *current* focused destination. We do not yet
implement the proposed Xwayland focus-generation observer or safe quick typing.
There remains a race with focus changes after user approval. Text delivery is
reported as **input queued**, not application consumption or message delivery.
A request is consumed once even if transport completion is uncertain; no retries.
Unavailable IME acquisition leaves the preview intact.

No VAD, always-listening mode, desktop transcription service, keyboard emulation
fallback, streaming-PC bridge, or automatic Enter. No general undo. Grip bindings
are not guaranteed globally active in every scene/dashboard state, and the app
never enables SteamVR's experimental overlay overrides on your behalf.

This is a compact prototype panel, not yet the proposed polished miniature status
chip. Font coverage/complex shaping, ergonomics, compositor cost, thermal/battery
impact and target application compatibility require further headset work.

## Critical runtime licensing boundary

Redux weights at `fad622f25f303105c20d70e201bcc477c88b620c` are CC-BY-4.0.
The installed **kestrel-kernels 0.7.0** license is different: proprietary, requiring
an M87 Labs written agreement for use; copying/redistribution depends on that
agreement. PyPI availability is not permission. See [third-party notes](third-party.md).

Do not publish a bundled Redux runtime, imply a public release is ready, or rerun
inference while applicable permission is unresolved. Our adapter is implemented;
that does not resolve distribution rights. An alternative runtime would be a
separately scoped and independently licensed implementation—not a silent model swap.

## Developer native build

Requirements: Linux, CMake/C++20, SDL3 >=3.2, Wayland client + scanner, FreeType,
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

## Deliberate launch, once runtime permission is settled

Explicit setup downloads only the pinned, openly licensed model:

```sh
python3 scripts/fetch-model.py --destination "$HOME/.local/share/frameyap-model"
```

`fetch-model.py` is idempotent for matching hashes, rejects existing mismatched
files, and is never invoked by build/tests or first utterance.

Provide your independently authorized CPU Python environment (moondream 2.4.0,
kestrel 0.8.0) and local weights:

```sh
./build-native/frameyap --run --assets "$PWD/assets" --font /path/font.ttf \
  --python /authorized/runtime/bin/python3 --worker "$PWD/python/frameyap/worker.py" \
  --model "$HOME/.local/share/frameyap-model" --threads 2 --socket gamescope-0 --head
```

Use a disposable text destination first. `--run` loads the model but does not
record until an explicit recording control. Click Insert only after focusing your
intended text field. Quit or SIGINT/SIGTERM closes capture, invalidates delivery
and terminates only the owned worker. Installer upgrades refuse an active app.

The worker API is intentionally small: a fork can replace the worker implementation
or add its own model/API integration without changing overlay and delivery code.
The default product remains local-only Redux; extending a fork does not authorize
sending existing users' audio to a service.

## Benchmarks

`scripts/benchmark-worker.py` accepts a supplied nonprivate PCM16/16 kHz/mono WAV;
no recording, device input or automatic download. `--show-text` is a separate
explicit disclosure of that fixture's transcript. Use only with an authorized
runtime. See the dated [POC record](evidence/poc-cpu-overlay-2026-09-24.md) for
measurements and their limits; they are not headset/performance acceptance.
