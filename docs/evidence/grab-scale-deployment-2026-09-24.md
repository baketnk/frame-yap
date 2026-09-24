# External grab/scale handles — 2026-09-24

Historical observation from an explicitly authorized Frame install/relaunch.
This record is not permission for later hardware runs and does not establish
physical drag/release behavior or human headset acceptance.

- Source commit: `33533ba8a2c85ccef3a388da4c2155e7dbbc14e0`.
- The user's Frame screenshot showed a thin grab underline below the terminal
  and an external lower-right corner bracket. FrameYap independently draws that
  layout in transparent RGBA margins; no Steam private UI code/assets are bundled.
  The screenshot remains outside Git.
- Local default, FreeType UI and native hardware-free suites passed 14/14,
  15/15 and 19/19 respectively. A synthetic panel preview was visually inspected;
  renderer tests check transparent gaps, opaque handle centers, antialiased alpha,
  cursor ownership and suppression of stale control approvals.
- Controller-ray math tests cover stationary stability, scaling on independent
  axes, rotated/relative geometry, out-of-bounds intersections and invalid rays.
  The original per-event resize feedback path was removed. Grab currently
  translates in the panel plane, not depth or orientation.
- Exact source archive SHA-256:
  `a8b8d4387de1fdd5cb1031e8905a6d6766e89616ceff3b62f902ea192a70e07b`.
  The checksum was verified before extraction on Frame. Native ARM64 Release
  build and hardware-free CTest passed 19/19; no test initialized OpenVR,
  recorded audio or injected input.
- Installed version: `2026-09-24T203812Z-g33533ba8`.
  Native-only external-runtime package SHA-256:
  `922c49de4188cba2e58bae829c02d5e9b8af3a33dba5bc6619b469389c8e73da`.
  No model or ASR runtime was bundled/downloaded. Staged `ldd` had no unresolved
  dependencies. Installed and staged binary SHA-256 both matched:
  `d77e4bec5b585a12a06885086c90c7d0e3b5b447d857b39a7f8586dfa7ce8518`.
- No old FrameYap binary was running at installation time. The managed installer
  selected the new version and retained `2026-09-24T201841Z-gc5e925cb` as previous.
  Installed `--version` and the relaunched process's executable path were verified.
  Only FrameYap was launched; no SSH, SteamVR or terminal session was stopped.
- Startup reported normal input priority, Lasers anytime off, and
  `panel-shown=Y`. This establishes successful initialization/show request,
  including the new intersection-mask call, not visual/physical acceptance.
  The normal runtime was left running for user testing; no recording or text
  delivery was deliberately triggered by this check.

Remaining acceptance: actual source-device reporting, grab/scale tracking,
release outside the mask, wrist/head behavior, transparency and comfortable
hit-target sizes. When legacy trigger release is not observable, dragging cancels
on loss of hover rather than relying on an outside MouseButtonUp event.
