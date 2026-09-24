# Technical provenance and limits

FrameYap is a standalone native POC. No unrelated application's source, build
tree, assets or Python environment is included. No model weights or runtime
binaries are in Git. The included Gamescope XML's provenance and separately
licensed build inputs are recorded in [third-party notes](third-party.md).
The dated [device probe](evidence/frameyap-apis-2026-09-24.md) records limited historical observations; its fixture code and private logs are
not part of this repository. No current device availability or release install
can be inferred from those observations.

## Model and preliminary measurements

Proposed model: [moondream/parakeet-redux](https://huggingface.co/moondream/parakeet-redux/tree/fad622f25f303105c20d70e201bcc477c88b620c),
revision `fad622f25f303105c20d70e201bcc477c88b620c`. An earlier local
inspection measured a 177,774,490-byte weight file. Its model card identifies
CC-BY-4.0; separate runtime/kernel redistribution terms must be reviewed before
packaging. No license for those artifacts is granted by this repository.

A preliminary, unpublished desktop benchmark used Python 3.12.13, moondream
2.4.0, kestrel 0.8.0 and four CPU threads. On an i7-13700K, warm decode
median/p95 were 61/137 ms and raw WER 11.11% on just 25 reviewed clips / 189
words. This is not a general accuracy estimate or Frame latency prediction. The
CPU-selected desktop process also occupied GPU memory; GPU-free operation was
not established in that earlier desktop trial. A later isolated ARM64 CPU-only
public-clip trial is recorded in [POC observations](evidence/poc-cpu-overlay-2026-09-24.md);
it does not establish live-microphone/headset acceptance or distribution permission.

## Public platform references

- [OpenVR 2.15.6](https://github.com/ValveSoftware/openvr/releases/tag/v2.15.6)
- [Gamescope 3.16.28 input-method protocol](https://github.com/ValveSoftware/gamescope/blob/3.16.28/protocol/gamescope-input-method.xml)
- [Gamescope IME implementation](https://github.com/ValveSoftware/gamescope/blob/3.16.28/src/ime.cpp)
