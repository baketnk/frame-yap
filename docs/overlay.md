# Native OpenVR POC panel

`src/overlay.hpp` provides RAII OpenVR ownership and `registration()`. The panel
only emits UI actions; `src/runtime.cpp` owns audio, transcription and insertion.
Native `--run` wiring in `src/main.cpp` is implemented behind the explicit
`FRAMEYAP_NATIVE` build option. Neither hardware-free tests nor a successful
compile establish Frame input, visibility, comfort or text delivery. Launching the runtime is explicit, never part of a
normal build or test.

## Rendering and controls

Explicit development dependencies: Valve OpenVR SDK v2.15.6 and FreeType 2.
Configure/build must not fetch them. An installed font is passed explicitly;
Unicode coverage depends on that font. The 900×500 RGBA, 0.85 m panel uses
`SetOverlayRaw` only when its status, detail, transcript, recording timer, or
preview page changes. The caller may call `draw(Panel)` at 10 ms intervals.
The complete transcript preview is paginated by glyph width and three-line
height; Prev and Next controls navigate it. Long status/detail messages display
a prefix and a visible truncation marker rather than disappearing silently.
`hand=true` attaches to the validly tracked left controller and falls back to
head-relative placement otherwise. Overlay pointer controls are Record
(click-to-start/stop), Cancel, Insert, Enter, Quit and Prev, plus Next in the
transcript area. Record is available to retry during startup/error; Cancel can
stop worker startup. Enter is *always* a separate deliberate action, not
inferred from text. Recording never automatically submits Enter.

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
Enter require an enabled panel; the clickable Record/Cancel controls remain
available while disabled. The action set has normal priority: global input
while a scene is active is not guaranteed, and experimental overlay overrides
are not switched on automatically.

The UI cannot itself guarantee a capture started when a BeginRecord action
arrives: the owning runtime checks worker readiness. `Cancel` invalidates
capture/worker work; a failed startup can be retried with Record. The
`UiAction::Toggle` enumerator remains for caller ABI compatibility but the
panel no longer emits it: Prev is navigation only. A pointer Record click
emits `Record` to toggle capture, unlike controller PTT edges.

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
