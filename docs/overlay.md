# Native OpenVR POC panel

`src/overlay.hpp` provides RAII OpenVR ownership and `registration()`. The panel
only emits UI actions; `src/runtime.cpp` owns audio, transcription and insertion.
Native `--run` wiring in `src/main.cpp` is implemented behind the explicit
`FRAMEYAP_NATIVE` build option. Neither hardware-free tests nor a successful
compile establish Frame input, visibility, comfort or text delivery. Launching the runtime is explicit, never part of a
normal build or test.

## Rendering and controls

Explicit development dependencies: Valve OpenVR SDK v2.15.6, Vulkan headers/loader
and FreeType 2. The native runtime needs a compatible system Vulkan driver.
Configure/build must not fetch them. The default font is the bundled Inconsolata
Regular, also used by kouseki; its OFL and extraction provenance are included in
[third-party notes](third-party.md). `--font FILE` overrides the JSON selection.
A missing selected font falls back to bundled Inconsolata, then a system DejaVu
Sans face if present. Glyph coverage depends on the selected face; full CJK
coverage is not claimed.

`src/panel_surface.*` renders **one 1000×680 RGBA canvas** for review, settings,
status and controls. `src/overlay_texture.*` uploads this CPU canvas into one
persistent Vulkan RGBA8 image and submits it with `SetOverlayTexture`. The image,
staging allocation and command buffer are reused; tabs do not create extra
overlays or render targets. The rounded mint-to-blue perimeter,
shallow curved accent, and dark cards borrow kouseki's VR visual language. Rounded
preview, status and control surfaces use independently rasterized antialiased edges
and restrained baked neon halos rather than GPU bloom. The recording indicator and
selected controls remain distinguishable by their labels, not color alone. Rounded
control hit areas exclude their clipped corners.
Rendering/uploads occur only for changed content, page or settings; laser hover
and button down/up are hit-tested without an upload. Static frames are reused.
The caller may call `draw(Panel)` at 10 ms intervals. Tracking transforms do
not require repainting the canvas.

The Vulkan instance/device enable the extensions requested by the running
SteamVR runtime and use its selected physical device and a graphics queue.
There is no desktop window, swapchain, SDL video dependency or raw-upload
fallback. Updates wait for the dedicated queue's previous upload and OpenVR
transfer before reusing staging memory. Image barriers finish in
`TRANSFER_SRC_OPTIMAL`, as required by
[OpenVR's Vulkan contract](https://github.com/ValveSoftware/openvr/wiki/Vulkan).
The queue is used on the overlay thread; GPU resources outlive `VR_Shutdown`.
The device selection, texture description and persistent panel-upload patterns
were compared with kouseki's `openvr_session.cpp` and `vulkan_renderer.cpp` at
`738569f4c41ff4c8fc9edd5bfff9c861957ea39e`; FrameYap owns this implementation.
GPU setup/submission errors stop startup or the run with an explicit error.
This replaces the raw-upload rendering path; headset flicker acceptance still
requires an on-device comparison. The
[Vulkan deployment record](evidence/vulkan-overlay-2026-09-24.md) documents the
native installation and offscreen GPU checks separately from headset acceptance.

The complete transcript preview is paginated by glyph width and four-line
height; Previous and Next navigate it without changing the source transcript.
Long status/detail messages show a prefix with a visible truncation marker.
The footer remains available on both tabs: Record (labelled Stop while recording),
Cancel, Insert, Enter, Quit. Record can retry after an error; it is disabled while
warming/transcribing and until an existing review is inserted or discarded.
Cancel can stop worker startup. Insert and Enter are disabled during recording
and transcription. A pointer action requires
a press/release on the same enabled control from the same cursor; focus loss,
tab changes, action-state changes and relocation clear pending presses. Enter is *always* a separate
deliberate action, not inferred from text. Recording never automatically submits Enter.

### User theme and controller configuration

Optional `$XDG_CONFIG_HOME/frameyap/config.json` (fallback
`~/.config/frameyap/config.json`, only with an absolute HOME) is read at native
overlay startup. The native binary does not create a config by itself; the
installer creates one with defaults on first install. Copy the shipped
`assets/config.example.json` to that path for manual installs. Example:

```json
{
  "font": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "theme": {
    "background": "#0c101b", "card": "#141c2b", "ink": "#e6f0f9",
    "muted": "#97adc1", "accent": "#1ff0a4", "warning": "#ff6e87",
    "frame_start": "#1fff91", "frame_end": "#1f70ff"
  },
  "buttons": {
    "ptt": "/user/hand/right/input/x",
    "left_grip": "/user/hand/left/input/grip",
    "right_grip": "/user/hand/right/input/grip"
  }
}
```

Each theme color is `#RRGGBB`; omitted colors keep the default. `font` is a
TTF/OTF file path (not a family name); a missing file uses the bundled font.
`buttons` maps named OpenVR actions (`left_grip`, `right_grip`, `ptt`, `cancel`,
`insert`, `enter`) to Frame physical `/user/hand/{left|right}/input/NAME`
button paths. Omitted actions retain their bundled defaults; an empty string
disables a mapping. Paths must be distinct. Only the Frame binding is customized;
SteamVR user overrides may still supersede it. On customized launches a generated
action manifest and adjacent bindings are placed in `$XDG_CACHE_HOME/frameyap/bindings`
(or `~/.cache/frameyap/bindings`); the bundled manifest remains unchanged. The
config is read once at launch, not hot-reloaded. On install/upgrade the installer
fills missing fields, removes retired keys, and resets invalid entries. It saves
the exact prior bytes under `config.json.backup-*` before a repair and refuses
symlink/oversized config paths; valid customizations remain intact. The installed
launcher no longer pins `--font`, so this selection takes effect. Direct native
launches with bad JSON, colors or button mappings fail startup rather than
silently changing input behavior. This does not enable experimental SteamVR
action overrides or prove delivery in games.

### Placement settings

First launch defaults to **World space**: a standing-universe absolute transform,
1.05 m ahead of the first valid headset pose, 0.16 m below eye height, upright
(yaw only). It stays there as the wearer moves. Startup waits for valid tracking
rather than placing a menu at the world origin. A tracking-origin reset requests
a fresh placement. Settings → Recenter in front deliberately resamples the pose.

Settings offers World space, Left wrist, Right wrist and Head on that same canvas.
World/head width is 0.85 m; wrist width is 0.42 m with mirrored controller-relative
offsets. A missing/untracked selected wrist temporarily falls back to world space,
with a visible explanation in Settings, then reattaches when tracking returns.
The saved preference is not replaced by the fallback. These offsets and sizes are
initial choices, **not headset-comfort acceptance**.

A selection saves only the mount token to `$XDG_CONFIG_HOME/frameyap/mount`
(or `$HOME/.config/frameyap/mount`), using an atomic replacement. No pose, audio
or transcript is saved. Missing/invalid settings default to world; write failure
keeps the selection for the session and displays a warning. `--mount
world|left-wrist|right-wrist|head` overrides the saved choice for one launch without
writing it; `--head` remains an alias for `--mount head`.

Settings also has **Lasers anytime** (default off). When enabled, FrameYap sets
OpenVR's `VROverlayFlags_MakeOverlaysInteractiveIfVisible` on its panel. OpenVR
requests system-wide laser mouse mode while the panel is visible, including
with Steam's dashboard closed; it may change interaction with games. Turning
it off removes that request. This is **not** the experimental overlay action
priority override and does not promise pass-through of a dashboard-owned
button or PTT delivery during dashboard focus. The choice is saved as `on` or
`off` in `$XDG_CONFIG_HOME/frameyap/lasers-anytime` (default
`~/.config/frameyap/lasers-anytime`) using a private atomic file replacement;
missing, symlinked or invalid files mean off. A failed save keeps the new
choice only for the running session and shows a warning. To turn it back on
after disabling it with the dashboard closed, open the dashboard to use its
laser on the Settings button. Controls-only checks can toggle it temporarily
but do not save the preference. This has not yet been accepted in a headset.

### Hardware-free UI checks

The default build tests mount parsing, laser preference persistence and pose
geometry without any native dependencies. A FreeType-only opt-in build exercises the actual renderer,
pointer gating, tab switches, pagination, recording state and redraw invalidation:

```sh
cmake -S . -B build-ui -DFRAMEYAP_UI_TESTS=ON
cmake --build build-ui
ctest --test-dir build-ui --output-on-failure
# Optional synthetic fixture previews; no OpenVR initialization:
./build-ui/frameyap_panel_test assets/fonts/Inconsolata-Regular.ttf /tmp/frameyap-ui
```

The last command writes `-review.ppm`, `-settings.ppm`, `-recording.ppm` to the
supplied prefix. The native build includes these tests too; tests never initialize
OpenVR or touch the real mounting preference. Physical pointing, tracking loss,
recentring and readability still require a separately authorized headset check.

The opt-in native `--check-controls` probe logs pointer counters and action
callbacks to the terminal rather than repainting them on the panel. Its canvas
stays static for Record/Cancel/Insert/Enter clicks so those clicks can be checked
without diagnostic texture uploads. Switching tabs or mount still updates
the visible panel. Diagnostics identify `renderer=Vulkan` and count
`textureUploads`; raw/file `ImageLoaded` events are not GPU upload completions.
The native CTest suite tests persistent image reuse, queued transfer ordering,
coherent/noncoherent staging memory and error cleanup against Vulkan fakes;
it does not initialize the Vulkan loader, a GPU or OpenVR for that test.

An optional offscreen check exercises the real Vulkan backend with eight
synthetic RGBA patterns, verifying exact readback and image reuse. It needs a
GPU/driver, is excluded from normal builds and CTest, and requires `--run`:

```sh
cmake --build build-native --target frameyap_texture_check
./build-native/frameyap_texture_check --run
```

It does not initialize OpenVR or establish compositor/headset acceptance.

`assets/actions.json` names six actions: left/right grip, PTT, cancel, insert,
Enter. `bindings_frame_controller.json` maps right X click to hold-to-talk PTT;
the grip bindings remain for optional remapping/diagnosis. In one dashboard
probe grips were inactive; a later controls-only probe delivered repeated right
X PTT BeginRecord/EndRecord callbacks. The wearer reports controller actions
are usable with Steam's dashboard closed, not with the dashboard itself open.
Neither probe used a microphone or established game-scene pass-through.
`bindings_knuckles.json` is an additional **Index/knuckles example only**.
Collisions with scene actions require
separate on-device validation. Left grip double tap
(releases <=250 ms, second press within 350 ms) requests explicit Enter only
when enabled. Right grip: first short squeeze and release (<=250 ms), then
second squeeze **down** within 350 ms starts capture; hold as long as needed
(up to runtime's clip bound), second **release** ends capture. The named PTT
action explicitly begins on down and ends on up. On tracking-pose invalidity,
action inactivity or overlay focus loss, a held capture emits Cancel, and
reconnection requires a neutral observation before any new press. PTT and left
Enter require an enabled panel; clickable Record remains available for retry
after an error and Cancel is always available. The action set has normal priority: global input
while a scene is active is not guaranteed, and experimental overlay overrides
are not switched on automatically.

The UI cannot itself guarantee a capture started when a BeginRecord action
arrives: the owning runtime checks worker readiness. `Cancel` invalidates
capture/worker work; a failed startup can be retried with Record. The
`UiAction::Toggle` enumerator remains for caller ABI compatibility but the
panel no longer emits it: Previous is navigation only. Pointer Record/Stop emits
explicit `BeginRecord`/`EndRecord`, just like PTT edges, so a stale Stop cannot
become a new recording when another input has already ended capture.

## Registration

`assets/application.vrmanifest.in` is a **template**, not a runnable manifest.
At install, substitute absolute executable and action JSON paths and store
it under the user-owned install directory, keeping bindings adjacent to action
JSON. Register `local.frameyap.overlay` using `registration(path,false,false)`;
autolaunch is opt-in. Unregister before deleting the installed manifest.
The app key is not a Steam store AppID. Registration uses OpenVR Utility init
only when explicitly invoked, then verifies `IsApplicationInstalled`. The device
required `binary_path_linux_arm` in the manifest (a generic or Linux-only path
was silently skipped despite successful AddApplicationManifest return).
Registration, repeated registration and removal were tested against the running
Frame runtime, with autolaunch verified off. The overlay explicitly identifies
its process with the registered app key before setting its action manifest.
Cold-runtime behavior and actual SteamVR-menu launch still need validation.
See [packaging](packaging.md); no published release is claimed here.

A live headset check must be opt-in and distinguish overlay API discovery from
controller delivery, actual transcription, insertion into a disposable target,
and human comfort/acceptance.
