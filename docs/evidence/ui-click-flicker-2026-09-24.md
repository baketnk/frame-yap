# FrameYap click flicker investigation — 2026-09-24

Dated controls-only observations on one user-authorized Frame. This is not
headset acceptance or proof that a compositor update will behave identically.
No microphone was opened and no text/Enter was delivered. Both checks used the
finite `--check-controls --mount world` mode; clicks were diagnostic only.

## What changed

The earlier diagnostic painted pointer counts, action text and controller state
onto the panel, causing `SetOverlayRaw` uploads even for clicks that would not
otherwise alter its pixels. Commit `b5499ec` logs those values to the terminal
instead; it keeps the panel static for Record/Cancel/Insert/Enter clicks, while
navigation and mount changes still repaint. A subsequent commit, `c59c8b1`,
counts application raw uploads, show/hide calls and OpenVR visibility/focus/image
events without adding repaints.

The rendering loop in `src/runtime.cpp` and `src/overlay.cpp` is single-threaded:
it draws before polling input and processes click actions before the next draw.
No separate logic/render thread race was found in this path. That alone does not
identify SteamVR's compositor behavior.

## Controls-only trials

- Static-canvas build `2026-09-24T173900Z-gb5499ec` was built on Frame from a
  clean snapshot; 11/11 hardware-free ARM64 tests passed. Its native-only archive
  was checksum-verified and installed; the previous version was retained. The
  30-second probe exited normally with 22 pointer downs, 22 ups and 8 action/
  mount/recenter hits. The wearer reported that the **whole overlay still
  disappeared on every action click**, including clicks that leave the canvas
  static. This contradicts full raw uploads being the *sole* cause of the flash.
- Event-counter build `2026-09-24T174700Z-gc59c8b1` was built on Frame from a
  clean snapshot; 12/12 hardware-free ARM64 tests passed. It was checksum-verified,
  installed and version-checked. The 30-second probe exited normally with 7
  pointer downs, 7 ups and 2 action/mount/recenter hits. From start to finish,
  application `ShowOverlay` calls stayed at **1**, `HideOverlay` calls at **0**,
  `VREvent_OverlayShown` at **1**, and `VREvent_OverlayHidden` at **0**.
  `SetOverlayRaw` calls rose from 2 initial uploads to 7 during the trial;
  image-loaded events reached 7 with no image-failed events. Four overlay and
  four global focus-change notifications were observed, but not on every click;
  input-focus-captured and gamepad-focus-lost counters stayed at zero. Pointer
  events occurred without intervening raw uploads or hide/show events. This
  second probe had no separate contemporaneous headset visibility report.

The second probe did not log hit targets; its five later raw uploads may include
tab, mount or placement-note changes and are not a per-action-click count. The
log does **not** establish that SteamVR
never briefly occluded the panel: it only shows that the app did not call hide
and received no hidden event in that finite trial. The first trial's visual
report plus static-click rendering behavior point toward dashboard laser/input
compositing rather than a FrameYap repaint race, but the exact compositor cause
remains unverified. A persistent GPU texture alone cannot explain or guarantee a
fix for flicker observed when no texture is uploaded.

The installed diagnostic version at the end of this investigation was
`2026-09-24T174700Z-gc59c8b1` (native-only, external ASR runtime unchanged); an
independent controls-only right-X binding trial occurred between these two
builds. No session or SSH process was terminated. No further hardware remedy is
claimed here.
