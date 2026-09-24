# Frame bindings and explicit Enter deployment — 2026-09-24

Historical evidence, not current device status or permission for further live tests.

## Implemented and checked

User approved right X hold-to-talk, B Cancel, A Insert + space, Y pending Insert
+ Enter; existing grip gestures remain. A Bindings tab shows OpenVR origin names
and requests the runtime binding editor. Insert ensures a trailing ASCII space;
explicit Enter consumes pending review, queues its text, releases that IME lease,
then acquires a new lease for Submit. Failed text never proceeds to Submit; an
uncertain send is not retried. No transcription completion auto-submits.

Read-only inspection of the installed Frame controller profile confirmed A/B/X/Y
on the right and a D-pad on the left. It references runtime-owned left/right SVG
diagrams for SteamVR's editor. FrameYap does not redistribute those diagrams or
claim a generic OpenVR button-glyph API. Editor display is not yet human-accepted.

Local default CTest: 13/13. Local native hardware-free CTest: 16/16. Synthetic
Bindings canvas was inspected for layout. These do not prove actual input delivery.

## Native install

Committed application source: `7a3a5451`, including wrist geometry/config work
`569ba13` and `e61bf73`. A Git source archive (excluding unrelated dirty workspace
files) built on Frame with existing standalone dependencies; ARM64 native
hardware-free CTest passed 16/16. No runtime/model dependencies were downloaded.

Installed and verified version: `2026-09-24T184918Z-g7a3a5451`.
Native-only archive SHA-256:
`9fa7991298731b27fc6dbf6d805167ee006e0b8ab93c2b859e2fad61007832a5`.
`current` selected this version and `previous` retained
`2026-09-24T183121Z-g69f5de5a`. Installer checked the supplied checksum. Installed
`--version` matched; `ldd` resolved bundled SDL/OpenVR and system dependencies.
The authorized runtime/model paths configuration hash remained unchanged.

The installer preserved existing disabled action values. With the user's approval
of the new controls, a separate exact-byte-backed-up config update enabled the
X/B/A/Y mappings and returned experimental input priority to normal. Lasers anytime
was already off. Saved `right-wrist` mount was retained; wrist config now uses
0.30 m width, zero roll and (0, 0.18, 0.089) offset.

A running FrameYap process appeared during configuration; only its exact verified
installed executable PID was terminated gracefully under the restart approval.
No SteamVR, SSH or user-session process was stopped. The restarted process's
`/proc/PID/exe` matched the new installed version. Startup reported normal priority,
Lasers anytime off and panel shown. This is API/process evidence, not headset
visibility or delivered-input acceptance. A normal app launch warms its configured
worker and opens/discards idle microphone samples; no recording or input-delivery
test was initiated by the assistant.

## Open acceptance

The wearer then reported **"wrist is backwards"**. Wrist orientation is therefore
not accepted; whether this means inverted text or a panel facing away was awaiting
clarification at this record. Do not count passing pose tests as comfort/orientation
acceptance. Actual B/A/Y delivery, text-plus-Enter ordering at a real target, and
SteamVR binding-editor behavior remain human-led checks.
