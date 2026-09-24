# Anchored resize / Auto Insert deployment — 2026-09-24

Historical observation from an explicitly coordinated Frame-only install and
owned-target fixture. It does not authorize further microphone tests or prove
actual speech-to-text Auto Insert, grip usability, arbitrary app delivery, or
human headset acceptance.

- Source milestones: `19a3db7` adds session-only corner-anchored resize;
  `c5e925c` adds default-off, fail-closed Xwayland Auto Insert.
- Local default/UI/native hardware-free suites passed: 13/13, 14/14, 18/18.
  Native Xvfb focus-loss test also passed 12 repeated runs. ARM64 native build
  from the `c5e925c` Git archive passed hardware-free CTest 18/18 on Frame.
  No normal test initialized OpenVR, recorded audio, or injected input.
- Native-only, external-runtime archive version
  `2026-09-24T201841Z-gc5e925cb` was staged with explicit licensed SDL/OpenVR
  files. SHA-256 archive:
  `c6bef87ec86fc0903247be714aec9c5784c3fa5ac788858dad9f50f1958d6f22`.
  The pre-install checksum check succeeded; system libxcb was available and no
  staged binary dependencies were unresolved.
- No FrameYap process was running when the installer checked. The idempotent
  user-local installer selected the new version, retained previous
  `2026-09-24T191513Z-g3c03f88f`, and backed up the existing config. Installed
  `--version` matched the archive version; installed/staged binary SHA-256 both
  `973fd119701093470b2ee3560df7e8049d968e1bab49f63f5242dabf055aa4ec`.
  Auto Insert remained `false`; no FrameYap process was left running.
- A separate temporary ARM64 test helper used the new `FocusGuard` and existing
  Gamescope IME `TextInput` against one project-owned Xterm receiver on display
  `:0`. It checked the expected window ID against X keyboard focus, X active
  window and Gamescope focused-window both before arming and after acquiring
  the IME lease. The helper queued the literal ASCII fixture `fy-safe-probe`
  without Enter; the private, disposable terminal receiver read **exactly 13
  bytes**, matching the fixture. Both temporary processes exited. No microphone,
  ASR model, arbitrary destination, speech transcript, or app UI Auto Insert
  workflow was exercised by that helper.

Compositor processing and the owned terminal's exact bytes support this narrow
fixture-delivery claim. The full microphone → worker → focus guard → Auto Insert
flow remains untested on Frame, as do physical resize interactions, input during
focus changes in actual games, and native Wayland targets. X focus checks are
not atomic with delivery; review remains the fallback. No public release or
bundled ASR runtime was published.
