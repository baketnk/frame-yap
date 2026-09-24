# Overlay flicker: source review and upstream reports — 2026-09-24

Local source review and public web research only. No Frame connection, OpenVR
initialization, microphone capture or input delivery was performed for this
investigation. No runtime fix or headset update is claimed. The earlier
[controls-only observations](ui-click-flicker-2026-09-24.md) remain separate
evidence; their device availability and permission do not carry forward.

## Does FrameYap recreate the panel on interaction?

The inspected `src/overlay.cpp` creates one OpenVR overlay in `Overlay::Impl`'s
constructor. Its only `DestroyOverlay` call is in cleanup, including startup
failure cleanup. Pointer handling, tab changes and `draw()` do not recreate that
handle. `PanelSurface` retains one fixed-size RGBA vector; normal redraws replace
its pixels, not its dimensions. This native panel is an OpenVR overlay, not an
SDL/X11/Wayland desktop window.

`draw()` calls `SetOverlayRaw` when `PanelSurface::render()` reports changed
content. Hover and button down do not invalidate the canvas; action clicks in
`--check-controls` only log diagnostics. Tabs and placement notes can still
repaint. In normal dictation, action-induced status changes repaint too.

The earlier event-counter trial recorded one show, zero hides and no hidden
events during interaction. The earlier static-canvas trial nevertheless had a
wearer report of whole-panel disappearance on action clicks. Thus application
handle recreation is unsupported by the code, and uploads cannot yet explain
all reported flicker. A compositor texture replacement or composition problem
could look like window recreation while the API overlay remains alive; this is
a hypothesis, not an observation of SteamVR internals.

## Relevant primary sources

- **OpenVR #772, April 2018:** a Linux C++ overlay author reported the entire
  overlay disappearing between `SetOverlayRaw` updates, with different behavior
  in the two eyes. Contributor Joe Ludwig advised using an OpenGL or Vulkan
  texture with `SetOverlayTexture` for frequent updates, describing substantial
  raw-upload latency and CPU/memory cost. This is a close match for redraw
  flashes, but is historical guidance, not a Frame measurement or confirmation
  that an overlay handle is destroyed.
  [Report and recommendation](https://github.com/ValveSoftware/openvr/issues/772#issuecomment-380539744).
- **OpenVR #941, November 2018:** Ludwig reiterated that raw uploads are a poor
  video path and recommended a graphics texture. This corroborates the API
  recommendation; it is not an independent reproduction of our click-only case.
  [Maintainer response](https://github.com/ValveSoftware/openvr/issues/941#issuecomment-440004776).
- **SteamVR 2.17.1 beta discussion, June 5, 2026:** Desktop+ developer
  `elvissteinjr` reported severe flickering with cursor override and the default
  cursor blob, plus problems with transparency and overlay ordering. FrameYap's
  inspected path does not use cursor override, so this is evidence of related
  compositor trouble, not an exact reproduction or a confirmed Frame bug.
  [Firsthand reports, comments 7 and 10](https://steamcommunity.com/app/250820/eventcomments/572665855469650962/).
- **Valve's SteamVR 2.17 release notes, September 10, 2026:** include fixes for
  dashboard/overlay cursor visibility and `MinimalControlBar` handling. These
  establish intervening changes after the June report; they do not identify a
  fix for FrameYap. Record the actual Frame runtime build before comparing it
  with these reports. OpenVR SDK v2.15.6 does not identify the running SteamVR
  version.
  [Official announcement feed](https://steamcommunity.com/app/250820/announcements/?l=english).

The GitHub web viewer omitted issue comments during this review; the linked
responses were checked through GitHub's public issues/comments API as well.

## What existing counters can and cannot establish

The pinned SDK defines `ImageLoaded` as completion of a raw/file image load,
not overlay creation. It separately defines `OverlayCreated` and
`OverlayDestroyed`. Shown/hidden events reflect API visibility, not proof that
every headset frame contains the panel.
[OpenVR v2.15.6 event definitions](https://github.com/ValveSoftware/openvr/blob/v2.15.6/headers/openvr.h#L853-L895).

Our existing totals do not timestamp each click, identify every hit target,
record lifecycle events, or resolve the named overlay again. Consequently they
cannot distinguish an internal compositor resource change from a render-order
problem. A constant application handle alone would not distinguish these either.

## Next controlled comparison, proposed only

First extend the finite controls-only probe with monotonic timestamps, hit
targets, raw-upload/image-load sequence numbers, and the current handle plus
read-only `FindOverlay` results. Record created/destroyed events with their
target handles: cursor or dashboard overlays must not be counted as FrameYap
recreation. Keep diagnostics off the canvas. Record the runtime version and
whether the flash affects one eye, both eyes, just the cursor or the whole panel.

Then compare one variable at a time with a contemporaneous wearer report:

| Trial | Purpose |
| --- | --- |
| Static canvas, default laser, diagnostic action clicks | Reproduce interaction without new raw uploads after startup settles. |
| Same static canvas, only `HideLaserIntersection` enabled | Test whether the compositor's cursor blob participates; this deliberately removes cursor feedback. |
| Same panel placed clear of dashboard surfaces | Test overlap/ordering without a renderer change. |
| Scheduled content updates with no pointing or clicking | Test raw-image replacement independently of interaction. |
| Same content updates through a persistent GPU texture | Compare the raw path with `SetOverlayTexture`, retaining and synchronizing the texture correctly. |

`HideLaserIntersection` suppresses the cursor blob; it does not disable mouse
input. `VisibleInDashboard` permits visibility there, whereas
`MakeOverlaysInteractiveIfVisible` activates global laser mode. These have
different effects and should not be changed together to diagnose flicker.
[Pinned flag definitions](https://github.com/ValveSoftware/openvr/blob/v2.15.6/headers/openvr.h#L3740-L3757).

A persistent GPU texture is a justified rendering experiment for content
updates, but cannot be promised to fix a static overlay blinking on clicks.
If static-click flicker survives the cursor/placement comparisons, the resulting
minimal reproduction is useful for an upstream compositor report. No report has
been submitted. Any native probe change still needs offline checks, authorized
deployment/version verification and an opt-in human headset check.
