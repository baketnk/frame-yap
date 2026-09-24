# Native OpenVR POC panel

`src/overlay.hpp` provides RAII OpenVR ownership and `registration()`. The panel
only emits UI actions; `src/runtime.cpp` owns audio, transcription and insertion.
Native `--run` wiring in `src/main.cpp` is implemented behind the explicit
`FRAMEYAP_NATIVE` build option. Neither hardware-free tests nor a successful
compile establish Frame input, visibility, comfort or text delivery. Launching the runtime is explicit, never part of a
normal build or test.

## Rendering and controls

Explicit development dependencies: Valve OpenVR SDK v2.15.6 and FreeType 2.
Configure/build must not fetch them. The default font is the bundled Inconsolata
Regular, also used by kouseki; its OFL and extraction provenance are included in
[third-party notes](third-party.md). `--font FILE` is an explicit override. Glyph
coverage depends on the selected face (no automatic system-font fallback or full
CJK coverage is claimed).

`src/panel_surface.*` renders **one 1000×680 RGBA canvas** for review, settings,
status and controls. One OpenVR handle receives it with `SetOverlayRaw`; tabs do
not create extra overlays or render targets. The rounded mint-to-blue perimeter,
shallow curved accent, and dark cards borrow kouseki's VR visual language, with
an independently implemented CPU renderer and a baked halo rather than GPU bloom.
Rendering/uploads occur only for changed content, page, settings, or pointer
feedback; static frames are reused. The caller may call `draw(Panel)` at 10 ms
intervals. Tracking transforms do not require repainting the canvas.

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

### Hardware-free UI checks

The default build tests mount parsing, persistence and pose geometry without any
native dependencies. A FreeType-only opt-in build exercises the actual renderer,
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

`assets/actions.json` names six actions: left/right grip, PTT, cancel, insert,
Enter. `bindings_frame_controller.json` uses the observed Frame profile's grip
click paths; both bound actions were reported tracked/active in the device check.
This does not prove physical gesture delivery. `bindings_knuckles.json` is an
additional **Index/knuckles example only**. Collisions with scene actions require
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
