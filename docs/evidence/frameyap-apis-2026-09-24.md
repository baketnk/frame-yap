# Historical Steam Frame API probes — 2026-09-24

These observations were made on one device and SteamOS build, not rerun as part
of this repository's scaffold. They do not prove current availability or headset
acceptance. No probe code, user audio, credentials or runtime binaries are shipped.
See the [design](../design.md) for proposed implementation and safety gates.

## Environment

SteamOS 0.4.0 (VR variant), AArch64, glibc 2.39, Python 3.12.3;
Gamescope 3.16.28-2. Linux ARM64 package availability is not evidence of
inference speed or compatibility with this device.

## Observations and limits

| Surface | Historical observation | Limit |
| --- | --- | --- |
| OpenVR | Native `VRApplication_Overlay` initialization succeeded; `IVROverlay_028`, `IVRInput_011`, `IVRSystem_026` and `IVRApplications_008` were accepted. | No rendered overlay, binding or autolaunch test. |
| Gamescope IME | `gamescope_input_method_manager` v3 advertised; v2 binding accepted and returned `done(serial=1)`. | Discovery is not delivered input. |
| Unicode delivery | A separate disposable X11/XIM receiver on the device accepted an exact mixed-script Unicode fixture via `set_string` and `commit` after checking focus on its owned window. | One Xwayland receiver, not general games, native Wayland or PC-streamed targets. No Enter was sent. |
| libei | Sender connected; seat advertised KEYBOARD, not TEXT. | No bound device or delivered key test. |
| XTEST / uinput | XTEST advertised; `/dev/uinput` was openable by the test account. | No injected XTEST key or virtual device. |
| Audio | PipeWire input device enumerated. | No microphone recording or model inference in this probe. |
| Portal | Existing portal introspection exposed no RemoteDesktop, InputCapture, Clipboard, ScreenCast or GlobalShortcuts interface. | Not a guarantee for future OS releases. |

The Xwayland receiver was destroyed after the test. No packages, services or
Steam settings were changed. No microphone audio was recorded. This is a
historical API probe, not a live availability check or install smoke test.

## Implementation cautions

Gamescope's [input-method protocol](https://github.com/ValveSoftware/gamescope/blob/3.16.28/protocol/gamescope-input-method.xml)
is private and version-sensitive; see its [implementation](https://github.com/ValveSoftware/gamescope/blob/3.16.28/src/ime.cpp).
The serial is not a verified focus-generation guard; a roundtrip is not a text
consumption acknowledgement. The inspected [`gamescope-type` example](https://github.com/ValveSoftware/gamescope/blob/3.16.28/src/Apps/gamescope_type.c)
interprets newline as Submit: do not use it as a transcript pipe.
Generic virtual-keyboard and data-control Wayland globals were not advertised
in this probe. Focus tracking and Steam keyboard coexistence need separate tests.

The proposed Redux model is pinned to revision
`fad622f25f303105c20d70e201bcc477c88b620c`; the weight file was
177,774,490 bytes in an earlier local inspection. ARM64 Python 3.12 wheels
appeared available, but no native model load or inference was performed on the
Frame. Runtime redistribution terms require separate review.
