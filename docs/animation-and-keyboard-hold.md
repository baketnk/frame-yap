# Animation opt-in and keyboard hold — 2026-09-29

Animated background now defaults off in native config, the example config and
installer defaults. Settings → Animated background writes the app's nested
`gradient.enabled` field without replacing other settings. Static theme colors
and the frame remain; existing explicit opt-ins are retained by ordinary upgrades.
For this user's requested rollout, the existing Frame config was backed up and
explicitly set off before restart.

Review → Open Keyboard sends `tnkboard --show` on short press. An 800 ms hold
sends `--recenter` once, suppressing the short action on release. Leaving the
button, resetting pointers, hiding, focus/tracking loss or entering panel drag
cancels the hold. The launcher uses fixed exec arguments, not a shell command.
The keyboard's matching command cancels placement/releases active input, shows
it and uses the current headset pose; saved keyboard size is retained.

Verification:

- Full native x86-64 suite: 34/34 passed.
- Frame ARM64 suite: 34/34 passed.
- Inspected synthetic Settings and keyboard-hold previews.
- Managed local artifact `0.1.202609290204` installed and launched on the Frame;
  prior version retained for rollback. Existing authorized speech runtime/model
  retained; the restarted process spawned its Redux backend worker using
  `~/.local/share/frameyap/models/redux`.
- Installed native executable SHA-256:
  `62587bed7f7edfc0c4f2d0ef7f2624eac915f2e9a9839b3de058200e6148ce65`.
- Final config check: FrameYap, tnkboard and tnkdraw animation disabled.
- No audio recording, key injection or human acceptance result was manufactured.
  Long-press compositor delivery and subjective headset behavior still need the
  wearer's check. This is a local development rollout, not a published release.
