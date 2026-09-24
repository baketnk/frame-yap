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

`src/panel_surface.*` renders **one 1080×780 RGBA canvas** containing a 1000×680
main panel for review, settings, status and controls, plus transparent right/bottom
margins for a thin grab underline and an external L-shaped scale handle. `src/overlay_texture.*` uploads this CPU canvas into one
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
The footer remains available on all tabs: Record (labelled Stop while recording),
Cancel, Insert, Enter, Quit. Record can retry after an error; it is disabled while
warming/transcribing and until an existing review is inserted or discarded.
Cancel can stop worker startup. Insert and Enter are disabled during recording
and transcription. A pointer action requires
a press/release on the same enabled control from the same cursor; focus loss,
tab changes, action-state changes and relocation clear pending presses. Enter is *always* a separate
deliberate action, not inferred from text. Insert appends a trailing space (without
doubling an existing trailing space). Enter inserts any pending review and then
queues Enter; with no pending text it queues Enter only. A failed text step never
proceeds to Enter. Recording never automatically submits. Auto insert, when
explicitly enabled, can queue text + space after transcription only under the
stable Xwayland focus guard described below.

### Bindings button

**Bindings** requests SteamVR's in-headset binding editor directly for the
current process/action set, without changing the FrameYap tab. SteamVR owns
remapping, persistence and the full binding view. The Review tab shows a request
or error note after the call. Opening it clears pending pointer presses and
rearms controller gestures from neutral.

OpenVR 2.15.6 provides `OpenBindingUI`. Frame's installed controller profile
references left/right SVG diagrams for SteamVR's own editor. FrameYap
does not copy runtime artwork into its package.
Editor availability and artwork rendering still require headset acceptance.

Normal priority with Lasers anytime off is the practical baseline: the wearer
reports controller actions with the dashboard closed and clickable UI with it
open. The menu does not enable global overrides or promise simultaneous access.

### User theme and controller configuration

Optional `$XDG_CONFIG_HOME/frameyap/config.json` (fallback
`~/.config/frameyap/config.json`, only with an absolute HOME) is read at native
overlay startup. The native binary does not create a config by itself; the
installer creates one with defaults on first install. Copy the shipped
`assets/config.example.json` to that path for manual installs. Example:

```json
{
  "font": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "input_priority": "normal",
  "advanced_debug": false,
  "auto_insert": false,
  "wrist": {"x": 0, "y": 0.18, "z": 0.089, "width": 0.30, "roll_degrees": 0},
  "theme": {
    "background": "#0c101b", "card": "#141c2b", "ink": "#e6f0f9",
    "muted": "#97adc1", "accent": "#1ff0a4", "warning": "#ff6e87",
    "frame_start": "#1fff91", "frame_end": "#1f70ff"
  },
  "buttons": {
    "ptt": "/user/hand/right/input/x",
    "cancel": "/user/hand/right/input/b",
    "insert": "/user/hand/right/input/a",
    "enter": "/user/hand/right/input/y",
    "left_grip": "/user/hand/left/input/grip",
    "right_grip": "/user/hand/right/input/grip"
  }
}
```

Each theme color is `#RRGGBB`; omitted colors keep the default. `font` is a
TTF/OTF file path (not a family name); a missing file uses the bundled font.
`advanced_debug` is a boolean (default `false`, not a string): an opt-in
request for full diagnostic logs. Full logs may contain speech/transcribed text
and local paths; **raw audio clips are not archived**. The Settings tab shows
an Advanced debugging ON/OFF toggle and warns that changing it restarts the
worker and cancels current work (including pending review). Changes take effect
immediately; failed saves show a warning and keep the selection for this session.
Detailed logs are bounded and owner-private; see [worker diagnostics](worker.md#advanced-debugging). The native
`save_advanced_debug(path, bool)` helper updates only this value in a valid
config, retaining other fields and formatting; invalid/unwritable configs are
left untouched and return failure. The installer backs up original bytes before
repairing invalid values, while valid `true` and `false` are retained.

`auto_insert` is a separate boolean, default `false`, also available as a
Settings toggle. Only a **new** recording arms it. It observes the Xwayland
display selected by `DISPLAY`; its root `_NET_ACTIVE_WINDOW` and
`GAMESCOPE_FOCUSED_WINDOW` must agree with the exact X keyboard-focus window.
Both properties and focus are rechecked after IME lease acquisition. A watched
focus-out, root focus-property change (even if the same window returns), window
destruction, held keyboard key, missing X display or any disagreement permanently
disarms that clip. The transcript then remains for explicit review/Insert.
Native Wayland focus and child text-field focus cannot be safely inferred here;
those cases fall back to review. No automatic Enter, speech commands or retry.
The compositor can still change focus in the gap between the final check and
global delivery, and IME commit is not an application receipt. This path has
offline synthetic focus tests and a separate owned-target IME fixture; live
speech-driven Auto Insert, target coverage and headset acceptance remain
unverified. The setting is preserved on upgrade
and a failed preference write applies only to the current session.

`buttons` maps named OpenVR actions (`left_grip`, `right_grip`, `ptt`, `cancel`,
`insert`, `enter`) to Frame physical `/user/hand/{left|right}/input/NAME`
button paths. Omitted actions retain their bundled defaults; an empty string
disables a mapping, including after an upgrade. The Frame defaults are right
X = hold-to-talk, B = Cancel, A = Insert + space, Y = Insert + Enter. Existing
configs with empty actions retain those disabled mappings; change them explicitly
or use SteamVR's binding editor. Paths must be distinct. Only the Frame binding is customized;
SteamVR user overrides may still supersede it. On customized launches a generated
action manifest and adjacent bindings are placed in `$XDG_CACHE_HOME/frameyap/bindings`
(or `~/.cache/frameyap/bindings`); the bundled manifest remains unchanged. The
config is read once at launch, not hot-reloaded. On install/upgrade the installer
fills missing fields, removes retired keys, and resets invalid entries. It saves
the exact prior bytes under `config.json.backup-*` before a repair and refuses
symlink/oversized config paths; valid customizations remain intact. The installed
launcher no longer pins `--font`, so this selection takes effect. Direct native
launches with bad JSON, colors or button mappings fail startup rather than
silently changing input behavior.

### Experimental controller input priority

There are two independent gates:

1. SteamVR's Developer setting **Enable global input from overlays** (called
   **Experimental overlay input overrides** in the SDK documentation) permits
   global action priority. FrameYap reads it and never changes it.
2. FrameYap config `"input_priority": "experimental"` requests
   `k_nActionSetOverlayGlobalPriorityMin` (`0x01000000`) for its existing action
   set. `"normal"` or an omitted field requests priority zero. Restart FrameYap
   after changing the config. Invalid values fail native startup; the installer
   repairs them to normal and backs up the prior bytes.

This applies to all controller sources bound to FrameYap actions, including
custom SteamVR bindings. It does not replace the action manifest or change the
physical button mappings. Bound sources can take input away from games or the
dashboard. The request stays the same with the dashboard open/closed and Lasers
anytime on/off so the experiment can compare those modes. Successful API calls
do not prove that controller actions arrive or that dashboard interaction works.

Native startup logs the requested priority and SteamVR permission separately.
The no-audio/no-delivery `--check-controls` probe also logs dashboard state, the
Lasers anytime flag, `IsInputAvailable`, panel visibility/focus, and activity,
press state and pose/role acceptance for all six actions. The laser flag is our
request, not a detector for every system laser. Compare the same bindings in
each dashboard/laser state; check pointer clicks and press/release through mode
transitions too. Return `input_priority` to `normal` and relaunch to end the
FrameYap experiment.

### Placement settings

First launch defaults to **World space**: a standing-universe absolute transform,
1.05 m ahead of the first valid headset pose, 0.16 m below eye height, upright
(yaw only). It stays there as the wearer moves. Startup waits for valid tracking
rather than placing a menu at the world origin. A tracking-origin reset requests
a fresh placement. Settings → Recenter in front deliberately resamples the pose.

Settings offers World space, Left wrist, Right wrist and Head on that same canvas.
World/head width is 0.85 m; wrist width defaults to 0.30 m. All mounts have
a thin grab bar below the main panel and an L-shaped scale bracket outside its
lower-right corner, visually modeled on the user's Steam terminal-window screenshot.
These are original FrameYap controls, not Steam private UI components. RGBA alpha
leaves their surrounding area transparent; an explicit OpenVR intersection mask
excludes empty margins from laser hit testing (the slender strokes have larger hit
targets). The extra canvas area does not shrink the main panel's physical width.

Hold the laser's primary click on the bar and move to translate the panel **in its
plane**; this does not change depth or orientation. Drag the corner bracket to scale
between half and twice the configured width. The original upper-left corner remains
fixed relative to the chosen mount. Both work on either tab and all mounts. Movement
is bounded to two meters per axis; mount changes/recenter clear movement, while mount
changes retain the size factor. Restart restores configured position/size defaults.

A drag freezes its initial plane and calibrates a ray from the source controller to
the initial hit. Subsequent tracked poses determine translation/scale, even beyond
the original texture bounds; changing overlay coordinates never feed back into the
drag. For head/wrist mounts, geometry stays in the anchor-device coordinate frame.
The event's controller is used, with the primary dashboard device as the single-cursor
fallback when the event omits it. No guessed controller or desktop pointer fallback.
Release, changed UI authorization, tracking loss, hidden overlay, relocation, invalid
ray geometry or a 15-second safety limit cancels the drag. If legacy trigger state is
observable on press, release is also checked through that state, including outside
the texture. Otherwise the drag cancels as soon as this overlay stops being the
hover target, rather than waiting for a potentially missing outside MouseButtonUp. Other pointer approvals and bound actions cannot fire during a drag.
Source-device reporting, out-of-bounds release, mask behavior and comfort still need
on-headset acceptance. The left wrist uses
VR Workspace's fallback watch-face axes: panel-right points toward the fingers
(controller -Z), panel-up points out of the back of the hand (controller +Y),
and panel-front points toward controller +X. The right wrist reverses panel-right
and panel-front (controller +Z and -X), keeping panel-up unchanged so it faces
inward with upright, unmirrored text. The controller-relative center is
(0, 0.18, 0.089) m, approximating the compact HUD's surface center: its
0.12 m wrist lift, 0.09 m bottom anchor and ~0.03 m panel-center correction;
Z combines the fallback 0.054 m wrist calibration and 0.035 m finger-back offset. This copies placement geometry, not
VR Workspace's avatar-dependent wrist calibration. Wrist-mounted panels now use
the same *behavior* as kouseki's watch HUD: fully visible while their entire
orientation is within 60° of an upright, viewer-facing panel; linear opacity
fade from 60° to 75°, then hidden (including laser interaction). Pitch, yaw and
roll contribute together; turning the wrist away or moving the head around it
changes the angle. OpenVR's overlay alpha changes without rerendering the panel.
World and head mounts do not fade. Missing headset tracking hides a wrist panel;
a lost wrist still uses the existing world-space fallback. This was implemented
independently with no kouseki library or runtime dependency. Headset readability,
fade feel and interaction at the threshold still need live acceptance.

To tune the selected wrist, set `wrist` in `config.json` as in the example above:
`x`, `y`, `z` are controller-local meters (each -0.3 to 0.3), `width` is panel
width in meters (0.15 to 0.6), and `roll_degrees` rotates about controller -Z
(-180 to 180) before applying the offset. These settings apply to both wrists,
are read at startup, and do not alter world/head placement. Invalid values
fail direct native startup; the installer backs up and repairs invalid entries.
A missing/untracked selected wrist temporarily falls back to world space,
with a visible explanation in Settings, then reattaches when tracking returns.
The saved preference is not replaced by the fallback. These offsets and sizes are
initial choices, **not headset-comfort acceptance**.

A selection saves only the mount token to `$XDG_CONFIG_HOME/frameyap/mount`
(or `$HOME/.config/frameyap/mount`), using an atomic replacement. No pose, audio
or transcript is saved. Missing/invalid settings default to world; write failure
keeps the selection for the session and displays a warning. `--mount
world|left-wrist|right-wrist|head` overrides the saved choice for one launch without
writing it; `--head` remains an alias for `--mount head`.

Settings also has **Lasers anytime** (default off). Open the dashboard to change
it when system-wide lasers are disabled. When enabled, FrameYap sets
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
but do not save the preference. The wearer reports that pointer clicks work,
while normal-priority controller actions become unavailable in system laser
mode, including when this preference is enabled. Experimental-priority
coexistence remains unverified.

### Hardware-free UI checks

The default build tests mount parsing, laser preference persistence, pose geometry
and captured-ray grab/scale math without any native dependencies. Drag tests cover
stationary stability, independent axes, rotated/relative mounts, out-of-bounds hits,
invalid rays and no feedback from prior updates. A FreeType-only opt-in build exercises the actual renderer,
pointer gating, tab switches, pagination, recording state and redraw invalidation:

```sh
cmake -S . -B build-ui -DFRAMEYAP_UI_TESTS=ON
cmake --build build-ui
ctest --test-dir build-ui --output-on-failure
# Optional synthetic fixture previews; no OpenVR initialization:
./build-ui/frameyap_panel_test assets/fonts/Inconsolata-Regular.ttf /tmp/frameyap-ui
```

The last command writes `-review.ppm`, `-settings.ppm` and `-recording.ppm` to the
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
after an error and Cancel is always available. The action set defaults to normal
priority; the experimental config request is described above. Neither priority
guarantees delivery while a game or dashboard owns input.

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
