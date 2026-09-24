# Guided Frame focus probe — 2026-09-24

Historical observation, not authorization for later live input or proof of target safety.
The wearer consented to an opt-in, disposable-target focus trial. Two project-owned
`xmessage` windows were created on Xwayland `:0` for 65 seconds, then closed by
the owning finite command. No microphone, transcript, input injection or SteamVR
session cleanup was used. Other SSH/user processes were left untouched.

An earlier read-only snapshot showed `/tmp/.X11-unix/X0` and `X1`, Gamescope
socket `gamescope-0`, and `_NET_ACTIVE_WINDOW` matching
`GAMESCOPE_FOCUSED_WINDOW` on `:0`. Brief snapshots of an owned test window
also showed disagreement between these root properties, so either property
alone is insufficient to authorize automatic typing.

The wearer selected A/B and reported doing several focus changes. Window A was
`0x3e00022`, B was `0x4200022` in that run. A 120 ms sampled observer saw
X keyboard focus, `_NET_ACTIVE_WINDOW` and `GAMESCOPE_FOCUSED_WINDOW` agree at
A, change to B, then return to A several times. A transition to a third window
`0x3c00003` was also observed. At other moments the X keyboard-focus/active
window IDs changed while Gamescope's focused-window property still named A.
The root property is a window ID encoded as CARDINAL, not a generation token.

These samples establish *observable correlation for these two owned Xwayland
windows*, not continuous seat identity across all targets, a focus-loss event
stream, transcript quality, delivered input, native Wayland coverage or headset
acceptance. The offline fail-closed observer subscribes to X property changes
and focus-out on the exact armed window; its own live behavior and actual IME
delivery still require a separate disposable-target validation. There is still
a non-atomic gap between final focus check and compositor input processing.
