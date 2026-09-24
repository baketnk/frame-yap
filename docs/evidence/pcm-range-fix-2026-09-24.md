# Captured PCM range rejection — 2026-09-24

Historical observations; not permission for additional recording or inference.

After the wearer enabled advanced debugging and retried speech, the private
worker log showed successful requests followed by an inference-stage exception:
`ValueError: PCM must contain finite samples in [-1, 1]`, raised by the runtime's
`_float_pcm` validation. No captured speech or full private log is reproduced here.
Both native submission and Python clip reading already rejected nonfinite values;
finite amplitude overshoot was not bounded. The precise source of that overshoot
(microphone gain, capture processing or resampling) was not measured.

Commit `019b813` saturates finite microphone samples to `[-1, 1]` after SDL
conversion, preserving in-range samples and clip length. It does not rescale
whole clips or accept NaN/infinity. `Worker::submit` independently validates the
range before clip-file/IPC mutation. Fake-device and IPC regression tests cover
both signs of overshoot, extreme finite values, exact boundaries, unchanged
ordinary samples, nonfinite rejection and subsequent capture recovery.

Local default CTest passed 13/13; local native hardware-free CTest passed 16/16.
The coordinated native Git snapshot also includes `3c03f88f`, which changes the
Bindings button to open SteamVR's editor directly. Its source diff was reviewed
before the combined build; no competing installer was run.

ARM64 native hardware-free CTest: 16/16. Installed/verified build:
`2026-09-24T191513Z-g3c03f88f`.
Native-only package SHA-256:
`d8433bc6e9e84ee9b54728d99cf1f7631dd999d6aee562a54cc0d04cad484716`.
Installed `--version`, `current` selection and the relaunched executable path
matched. `previous` retains `2026-09-24T190806Z-g6592e1af`.

User config and authorized runtime/model paths hashes were unchanged across
installation; advanced debugging remained enabled by the user. No FrameYap
process was running at the pre-install check. Only FrameYap was launched;
no SteamVR, SSH or user-session process was stopped. The new diagnostic log
was mode 0600. No assistant-triggered recording, private-audio replay, public
fixture inference or input injection was performed. Actual speech retry after
this fix, recognition quality and headset interaction remain wearer-led checks.
