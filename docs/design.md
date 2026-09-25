# FrameYap native dictation overlay — design and remaining goals

Standalone project design. This document is the full target design, not a blanket
implementation claim. The native app now implements overlay/actions, bounded SDL3 capture, a persistent
Redux adapter, review-first Gamescope insertion and a user-local archive installer
with explicit source-build option. Redux's pinned manifest, offline status CLI,
manifest-driven dispatcher and in-panel Models selection/confirmation are
implemented locally. The installed chooser/installer handoff is **not yet
end-to-end verified on Frame**; the native-only archive needs an external CPU
runtime for speech.
Opt-in conservative Xwayland focus tracking is implemented locally but not yet
accepted for live automatic typing on Frame. A separate status chip and full hardware
acceptance remain possible future work. See [build and scope](build.md) and [third-party notes](third-party.md).
Future work is tracked in [TODO.md](../TODO.md).

## Recommendation

Build a small native ARM64 **OpenVR overlay application**, independent of
other applications, with **local Parakeet Redux CPU inference**. Use Gamescope's
input-method protocol for literal Unicode text; keep Linux key-injection APIs
as explicit fallbacks. No desktop ASR server, network hop, LLM cleanup, scene
renderer, avatar, desktop capture or root service is needed in the primary path.

A first-class product goal is a **one-command GitHub install without a Steam store
AppID**. Package a prebuilt native executable; the current native-only archive requires a
separately supplied, compatible CPU runtime (no automatic pip install). A future
isolated runtime bundle requires its own license and compatibility audit. Use a normal
OpenVR application key for registration, not Steamworks. Installation must remain
user-local with opt-in autolaunch. See [installation design](install-design.md).

```text
controller PTT / overlay mic button
          ↓
SDL3 → PipeWire/PulseAudio mic → bounded 16 kHz mono clip
          ↓
one persistent local Parakeet Redux CPU worker
          ↓
correlated literal transcript → focus/delivery policy
          ↓
Gamescope IME set_string + commit → focused Frame application
          ↘ overlay preview / error / explicit Type when delivery is unsafe
```

The CPU worker is local process isolation, not remote inference or a service
framework. The locally implemented manifest dispatcher uses a small bounded
transport owned by this repository; only Redux inference is shipped. Keep the
model loaded between utterances; do not spawn Python/load 178 MB for every release.
A correlated bad transcript is a request-level failure, not a model unload.

## Minimal interaction

The implemented menu consolidates review, status, controls and settings onto one
rendering surface, with world-space placement by default. Settings offer left
wrist, right wrist and head mounting. A smaller status-chip presentation while
armed remains a possible refinement, not a second implemented overlay. See
[the current UI and placement behavior](overlay.md).

```text
[ mic ]  Ready · On-device                     [ settings ]
          Target: selected application

Recording…  00:04           [ Cancel ]

"The recognized text appears here."
[ Type + space ]  [ Discard ]     [ Type + Enter — explicit ]
```

- States: warming, ready, recording, transcribing, review, input queued,
  unavailable/error. Input queued is not an application receipt. Recording uses
  visible icon + text, not color alone.
- Default Frame bindings: hold right X to speak, release to finish; B cancels,
  A types with a trailing space, Y opens/cycles the Quick phrases selection.
  Overlay Type + Enter or left-grip double-tap submits the selected literal + Enter,
  or pending review (normally + space) then Enter, or Enter alone if neither is present.
  The alternate right-grip tap-then-hold gesture and left-grip double-tap Type + Enter
  remain available. All are remappable through the Bindings button's SteamVR editor.
  A click-to-start/stop overlay button provides
  a binding-independent alternative. Bound recording to 20 seconds; discard
  accidental taps (initial threshold: 200 ms).
- **Auto insert (opt-in, locally implemented):** type on completion only when
  uninterrupted Xwayland target observation remains valid. **Review mode (default):**
  wait for Type. Bring up review on uncertainty rather than silently losing a
  transcript or typing into a new target. Live Frame acceptance remains open.
- Type + Enter requires its own explicit control activation: type pending review
  (normally with a trailing space), then queue Enter only if the text step succeeds.
  At the 4096-byte bound, preserve the entire transcript without a suffix if no
  space fits. With no review, it queues only Enter. Never interpret "submit", "delete" or other speech
  as commands. Transcription completion never auto-submits.
- No generic "undo last dictation" initially: another application's edits/cursor
  cannot be reliably rolled back by a guessed number of backspaces.
- Stop/disable releases owned keys and microphone, invalidates pending delivery,
  and terminates only the owned worker. No Steam/session/SSH cleanup commands.

## Host and rendering boundary

Implement the standalone `frameyap` executable, initialized
with `VRApplication_Overlay`. Frame accepted that application type and
`IVROverlay_028` in the probe. Use one `CreateOverlay` handle, an absolute world
transform or tracked-device-relative mount, `ShowOverlay`/`HideOverlay`, and
`PollNextOverlayEvent` for the panel.
Use SteamVR's overlay interaction rather than inventing scene controller rays.

Do not link an external scene host or launch another application behind the
panel. A small overlay-specific RAII owner in this repository should manage OpenVR, overlay
handles, input manifest and shutdown. No dependency on external application
libraries, assets, build trees or Python environments.

Use one small RGBA panel updated only on UI changes and a bounded recording
indicator cadence. The native implementation uploads the CPU-rasterized panel to
a persistent Vulkan image and uses `SetOverlayTexture`; the initial
`SetOverlayRaw` proof path has been replaced. No stereo
eye targets or per-eye scene rendering. Keep tracking in compositor transforms,
not an application-rendered hand-pose animation loop. Do not promise a particular
GPU cost until measured.

A scene renderer's canvas/MSDF resources are not an OpenVR overlay backend and
are not imported here. The bundled Inconsolata TTF is under its retained OFL; the panel renderer is
original FrameYap code. Further source/asset reuse requires an explicit
license-reviewed extraction, never a runtime path into another project's checkout.

### Controller bindings

Expose PTT, cancel and explicit Type/Type + Enter as named SteamVR actions; let the
user bind them. Do not assume a scene app's left-bumper mapping works globally or
silently takes a game's button away. Check action activity and neutral rearm.

OpenVR 2.15.6 documents experimental overlay action-set priorities
`0x01000000..0x01FFFFFF`, gated by SteamVR's **Experimental overlay input overrides**
setting. FrameYap now requests the minimum experimental priority when its config
has `"input_priority": "experimental"`; the default is `"normal"` (priority zero).
This can selectively override scene input, but delivered input and dashboard
coexistence still need testing on Frame. FrameYap only reads the SteamVR permission
setting and never toggles it automatically. Validate bindings with a scene active, dashboard
open/closed, lost tracking, and reconnection. Overlay interactivity/input ownership
is distinct from OS keyboard focus.

## Text delivery: use the proven path first

### 1. Gamescope IME — primary Frame backend

Connect to the discovered Gamescope socket; bind
`gamescope_input_method_manager` and `wl_seat`, handle unavailable/done events,
then issue `set_string(valid_utf8)` and `commit(last_serial)`. A v2 binding is
sufficient for text and actions even though the current server advertises v3.
Use generated bindings from the pinned XML, not a production handwritten wire
protocol. Own/destroy the IME object deliberately and release it while disabled.

This delivered an exact mixed-script string into the disposable Xwayland
receiver. It does not need clipboard ownership, sudo, `uinput`, application
plugins, or a new input daemon. However it is a **private Gamescope extension**;
isolate it behind a version-gated backend and test after SteamOS updates.

The server implements text through synthetic key events and a temporary keymap,
not a guaranteed rich-text/IME edit operation in every app. Verify actual target
toolkits and games. Respect singleton/unavailable handling and Steam-keyboard
coexistence. Never use the installed `gamescope-type` CLI as a transcript pipe:
its inspected sample loop is byte-oriented and interprets newline as Enter.

### 2. Explicit fallbacks, not a framework built up front

- **X11 clipboard + XTEST paste:** useful for apps that reject direct Unicode
  key events but accept paste. Own the appropriate X selection (CLIPBOARD or
  PRIMARY), serving the `UTF8_STRING` target on the destination X server;
  select the app's paste chord explicitly. Do not assume Ctrl+V in terminals.
  Do not overwrite/restore arbitrary clipboard history silently; expose Copy as
  a deliberate fallback. This backend has not yet been exercised on Frame.
- **libei:** current server accepts a sender and advertises KEYBOARD but **not
  TEXT**. Suitable for evdev-style key chords, not automatic Unicode insertion.
  Bind/resume/device lifecycle must still be tested. Direct EIS socket access is
  Gamescope-specific here; no portal RemoteDesktop route was exposed.
- **uinput:** current account can open it without sudo. Reserve for raw virtual
  keyboard needs; creating/retiring a device is unnecessary for primary dictation.
  Unicode is not an evdev keycode, so this alone does not solve text delivery.

Do not assume generic labwc virtual-keyboard/data-control protocols are present:
those globals are absent. Fail visibly if the primary backend is unavailable;
no automatic privilege escalation or OS package/configuration edits.

### Target/focus policy

The IME serial is **not** an established target-generation guard. Input goes to
the seat's current focus. No general Linux input API makes insertion into an
arbitrary app atomic with a focus check.

For the first supported route, use an explicitly armed Xwayland destination.
Track display identity, active top-level, actual X keyboard-focus window and a
monotonic focus generation; invalidate on focus loss/regain, window destruction,
disconnect or target change. Observe changes throughout capture/inference and
recheck immediately before delivery. Do not restore another app's focus behind
the user's back. Refuse quick insertion when modifier keys are held or target
identity is ambiguous. A matching final window ID alone is insufficient.

Gamescope exposes focus-display/window root properties, but their encoding and
relationship to seat focus need implementation-specific validation. Do not infer
that X display `:0` is always the destination, or that an X focus observation
identifies a native Wayland text field. For unobservable native Wayland focus,
keep the transcript in review; current paced manual Type also refuses an
unverifiable target. Do not advertise native Wayland target support.

Before the first send, focus failure keeps the result in review. A fresh Type
approves a currently verified Xwayland destination and creates a new delivery
authorization. The paced queue rechecks that same target before each <=24-codepoint
batch and explicit Enter, spacing commits by at least 150 ms. After a partial
send, focus failure drops the remainder and Enter without replay; it cannot
undo already queued input. The clipboard remains untouched. This minimizes stale
delivery but does **not** eliminate a race
between the final check and global input processing; do not claim otherwise.
A Wayland roundtrip means compositor processing, not application consumption.
Report `input queued`, never `message sent`.

Validate UTF-8 and enforce the 4096-byte bound; flatten line breaks/tabs, reject
NUL, escape and other control characters. No shell evaluation, commands, Enter,
terminal escapes or action inference from recognized text. One correlated request
may deliver at most once; cancellation invalidates it before any later reply.

## On-device Redux

Use the **same** `moondream/parakeet-redux` revision from the benchmark:
`fad622f25f303105c20d70e201bcc477c88b620c` (177,774,490-byte weight file), initially
moondream 2.4.0 / kestrel 0.8.0. Do not substitute dense Ultra or silently fall
back to desktop/cloud inference.

The earlier benchmark used this local API (its source belongs to the originating
repository, not this project):

```python
model = md.photon("moondream/parakeet-redux", model_path=local_model_directory,
                  device="cpu", cpu_threads=thread_budget)
text = model.transcribe(audio=mono_float32, sample_rate=16000)["text"]
```

Linux ARM64 Python 3.12 native wheels and a packaged CPU payload exist. That is
sufficient reason to **try the native CPU path first**, not proof of Frame
performance. Its 61/137 ms desktop median/p95 must not be reused as an estimate
for the headset. Its historical dictation quality was worse than Small.en on
that tiny dataset; keep transcript visibility and a cheap retry.

Implement a small independent audio/worker adapter with explicit capture,
single-request bounds, owner-only runtime files, correlated replies and cancellation.
The worker is independently implemented; do not link, vendor or import another
application's speech code. A narrow manifest dispatcher now selects a checked
local backend launcher; this is not a general cloud/provider framework and only
Redux is supplied with a runtime implementation. Other manifests require their
own audited offline engine, launcher and tests.

Suggested ownership, introduced only as implementation needs it:

- `src/overlay.*`: OpenVR lifetime, panel presentation and controller actions.
- `src/audio.*`: explicit SDL capture and bounded mono PCM.
- `src/worker.*`: one local child process, bounded requests/replies, timeout/reaping.
- `src/text_input.*`: Gamescope protocol, focus observation and delivery policy.
- `python/frameyap/`: persistent CPU Redux worker, no external application imports.

The worker reads a fixed private clip and returns an ID-correlated literal string;
one request at a time, 64 KiB framed messages, 4096-byte transcript, and a bounded
processing timeout. No shell commands in IPC and no input authority in the worker.

- One persistent worker and one loaded model; no queue of utterances. Initially
  batch at PTT release, not speculative streaming or endpoint/VAD delay.
- Start with **two CPU threads**, compare against four on Frame; bound Torch and
  native kernel pools. Choose measured latency versus compositor contention,
  not the desktop's thread count by habit. No real-time scheduling or permanent
  CPU pinning initially; inspect runtime affinity behaviour during measurement.
- Explicit local model path and offline loading. Pinned files and attribution live
  in `assets/backends/redux.json`; `python/frameyap/model_files.py` validates its
  schema and hashes. The CLI's offline `--list-models`/`--check-model` checks
  never start inference. Settings → Models selects/saves the manifest ID and
  restarts the worker, invalidating any audio/review/delivery authorization.
  Install requires a separate confirmation showing source, size, license,
  attribution and exact-manifest SHA-256; only that click hands off to the
  installer for pinned model files. Missing runtime/weights produces an
  actionable error, not an unsolicited download/network fallback.
- Native `--run` explicitly starts offline verification then warms a verified
  selected local model before PTT; warming must not record audio. Missing or
  unverified files disable Record rather than prompting a background download.
  Keep model reuse after normal completion and request-local transcript errors;
  cancellation may restart the owned worker.
- Use a private owner-only directory under `$XDG_RUNTIME_DIR` for bounded
  tmpfs-backed clips; remove them on completion, error, cancellation and shutdown.
  Avoid persistent audio/transcripts by default. Local IPC is not a network hop;
  do not add shared-memory complexity before measuring it.
- Use CPU-only Torch where supported; test native kernel import/model load with
  no CUDA device/runtime assumption. Do not copy the desktop's x86 venv.
- Weights are ~178 MB; Torch, kernels, temporary conversion and activations mean
  install size/RSS will be larger. Measure cold load, peak RSS and package size.
  Keep runtime notices separate from model attribution.

Microphone access does not mute VRChat or any other social-voice app. Shared
PipeWire capture may let both hear the same utterance; the overlay must not claim
private dictation unless the other app's transmission is separately muted. No
automatic global microphone mute/reroute in this scope.

## Delivery milestones and acceptance gates

These are proposed implementation gates, **not completed acceptance**:

1. **Offline ARM64 CPU spike:** isolated user-directory environment, pinned local
   model, permitted runtime distribution. Run known nonprivate clips, then the
   consented benchmark clips only if explicitly made available within their data
   scope. Record cold load, first/warm p50/p95, errors, peak RSS, two/four threads and dependencies.
   Confirm no required NVIDIA/desktop connection. Stop and report if the packed
   CPU runtime is incompatible; do not silently expand to dense weights.
2. **Minimal overlay:** render status/review panel while another scene stays active;
   confirm dashboard/hand placement, input events, close/reopen and no scene-focus
   takeover. Validate global PTT separately rather than blocking the clickable
   prototype on experimental override support.
3. **Real dictation path:** microphone → local Redux → preview → explicit Type
   into a disposable target; then validate opt-in Auto insert after target tracking tests.
   Test Unicode, punctuation, long bounded clips, silence, cancellation, duplicate
   replies, lost mic, worker crash and missing model without persisting speech.
4. **Target matrix:** Xwayland terminal/browser and selected native/Proton game
   text fields; Steam keyboard coexistence; native Wayland targets separately.
   Test focus changes during capture/inference, rapid loss/regain, held modifiers,
   explicit Type retargeting, no hidden Enter, and no second delivery.
5. **In-headset acceptance:** readable feedback and comfortable PTT; measured
   release-to-insert latency and compositor timing while an actual scene runs;
   no noticeable sustained thermal/battery regression. Set numeric budgets after
   the ARM64 spike, not from an x86 result. Physical success remains human-led.

Native Frame app typing is the first scope. Local keystrokes may or may not be
forwarded by the PC-streaming client; that route needs its own test. If necessary,
a future optional **text-only** host bridge could send the completed transcript,
without moving recognition off Frame. It is not part of this initial design.

## Standalone dependency and test policy

The default hardware-free build needs only CMake and a C++20 compiler (Python
runs additional offline tests). `FRAMEYAP_NATIVE=ON` explicitly selects OpenVR,
SDL3, FreeType and Wayland client/generated protocol bindings. A separately
authorized Python Redux environment is explicitly supplied at launch; the
native-only installer neither bundles it nor pip-installs one. Pin revisions
and review licenses for each introduced dependency. No automatic fetch/install in configure or normal tests; no external checkout discovery.

Hardware-free tests should cover state transitions, bounded PCM/transcripts,
worker framing/timeout/cancellation, duplicate/stale replies and focus generations
with fakes. Separate opt-in Frame checks cover protocol availability, owned-window
insertion, microphones, inference and overlays. No fixture establishes physical
headset acceptance.
