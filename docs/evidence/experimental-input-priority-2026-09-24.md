# Experimental overlay input priority — 2026-09-24

Source commit: `69f5de5a`.
Installed native ARM64 version: `2026-09-24T183121Z-g69f5de5a`.

The user confirmed the Vulkan flicker fix, reported controller actions becoming
unavailable in system laser/dashboard interaction states, and agreed to try
OpenVR's experimental priority mechanism. The investigation concerns action
delivery across modes, independent of any specific physical button.

## Implementation and preparation

FrameYap config `input_priority` accepts `normal` (default) or `experimental`.
Experimental requests `k_nActionSetOverlayGlobalPriorityMin` (`0x01000000`) for
the existing action set through `UpdateActionState`, including while system
laser mode or the dashboard is active. The request covers the sources bound to
FrameYap actions; bindings and action paths are unchanged. The installer
preserves the selection and backs up invalid config before repair.

SteamVR separately permits global input priority through its Developer setting.
The current Frame's saved `steamvr.globalActionSetPriority` was already true;
its runtime default was false. This was a targeted file read, not an effective
runtime API query. FrameYap now reads this permission through `IVRSettings` and
reports it separately from its priority request at startup. It does not write
the SteamVR setting.

The controls-only diagnostic reports all six actions' activity, press state and
pose/role acceptance, alongside dashboard visibility, our Lasers anytime flag,
`IsInputAvailable`, panel visibility and the application focus gate. This can
distinguish runtime action inactivity from application rejection. The laser flag
records our request; it is not a detector for all system laser activation.

## Verification and deployment

- Local default build and CTest: 13/13 passed.
- Local native build and hardware-free CTest: 16/16 passed. The fake Wayland
  test used sandbox escalation to bind its local Unix socket.
- Frame ARM64 Release build from a checksum-verified archive of the source
  commit above: 16/16 hardware-free CTest checks passed.
- The package was installed through the existing installer. Installed
  `--version` matched and its executable hash matched the staged executable.
- FrameYap's config was set to `experimental` under the application lock, with
  an exact backup; all other config values were verified unchanged by that edit.
- No OpenVR probe, microphone capture, inference or text delivery was started.
  No application, SSH or session process was stopped.

Native archive SHA-256:
`40a9445427469286e8997563bc5598ace7769ba4c13b1a9290911c821d0e419c`.
Installed executable SHA-256:
`da842efcad926660efc66c03d843592573380b30e563e797df2944a0d6d77da7`.

The prior Vulkan version `2026-09-24T181314Z-g4c043c99` is retained. To end the
priority experiment on the new build, set `input_priority` to `normal` and
relaunch. To roll back to the older binary, first restore the pre-upgrade config
backup `config.json.backup-sih_efaa`: that binary predates the new config key.
The separate `config.json.backup-priority-2026-09-24T183121Z-g69f5de5a` preserves
the upgraded config before selecting experimental priority.

## Acceptance boundary

The user was told the new build is ready for headset testing. Compare the same
bindings with the dashboard open/closed and Lasers anytime on/off; check both
pointer interaction and controller press/release, including mode transitions.
Higher priority may consume input used by a scene or the dashboard. This record
does not claim that actions now arrive in every state or that simultaneous
dashboard interaction is accepted. Dated deployment evidence does not establish
future device availability or authorize future live runs.
