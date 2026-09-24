# FrameYap POC observations — 2026-09-24

Dated observations on one user-authorized Steam Frame, not proof of future device
availability, full dictation acceptance or runtime redistribution rights. No audio,
weights, private transcript/log files, credentials or runtime binaries are included.
The final native package below was built from source commit **`d60d8d2`**.

## CPU compatibility trial (before runtime-license review)

Isolated user-local environment: ARM64, glibc 2.39, Python 3.12.3,
moondream 2.4.0 / kestrel 0.8.0 / kernels 0.7.0 / native 0.1.8,
Torch **2.8.0+cpu** (`torch.version.cuda is None`). Model weights/config/tokenizer
verified against pinned Redux revision `fad622f25f303105c20d70e201bcc477c88b620c`.
No dense model substitution or desktop/cloud inference was used.

A one-second synthetic silence trial loaded in 5.645 s; three decodes took
0.187 / 0.149 / 0.167 s and returned no text. Peak process RSS in that trial was
989,844 KiB (~967 MiB). This is not peak RSS for arbitrary 20-second speech clips.

Public 11.0-second JFK speech fixture:
<https://github.com/openai/whisper/blob/v20250625/tests/jfk.flac>.
Source FLAC SHA-256 `63a4b1e4c1dc655ac70961ffbf518acd249df237e5a0152faae9a4a836949715`.
Converted to PCM16/mono/16 kHz WAV with ffmpeg; WAV SHA-256
`0c397b12d7dbdd89e4fd0d9a0840c14a2c5c3707758562755cda96a931b25a91`.
The persistent worker used bounded file/pipe IPC; no microphone was opened.

| CPU threads | Worker load | Five request times (seconds) | Median | RTF | Audio/compute ratio |
| --- | --- | --- | --- | --- | --- |
| 2 | 5.190 s | 1.550, 1.619, 1.587, 1.619, 1.659 | 1.619 s | 0.147 | 6.8× realtime |
| 4 | 4.655 s | 1.196, 1.158, 1.143, 1.249, 1.502 | 1.196 s | 0.109 | 9.2× realtime |

The first result reproduced the fixture's known sentence; all five replies in
each run were correlated and 108 UTF-8 bytes. These are medians/maxima from one
public clip, not representative accuracy/WER, p95, streaming realtime performance,
or compositor/thermal acceptance. Load time is excluded from RTF. Two threads
remain the default pending scene-contention measurements.

An unconstrained dependency install initially selected CUDA Torch/NVIDIA wheels;
these were replaced/removed from the owned venv before measurement. No system
packages were changed. Later inspection found the proprietary kernel license's
separate-agreement requirement. Permission was declared unresolved; further
inference and proprietary-runtime packaging were paused pending vendor clarification.
See [license boundary](../third-party.md). The acquired development environment
remains user-local and is not included in the native-only installation.

## Native runtime / controller / overlay checks

- Native CMake build succeeded on Frame using standalone OpenVR SDK v2.15.6,
  explicitly built SDL 3.2.16, system Wayland and FreeType. No unrelated app tree
  or environment was linked/copied. No sudo or system/session restart.
- Installed executable connected to `gamescope-0`, bound the IME and received
  ready/done state; each discovery check disconnected **without text or Enter**.
- OpenVR accepted the raw panel. The user reported seeing the five-second window.
  This confirms visibility, not readability/comfort or successful pointer clicks.
- Device controller properties identified `frame_controller` and its input profile.
  The profile exposes grip `click`; authored default bindings were added. A
  controls-only run reported both grip actions tracked/active. **No physical
  gesture delivery was observed in that run.** Double-tap/hold/loss behavior is
  currently verified by deterministic unit tests, not a human controller trial.
- The ARM loader initially tried `/data/work/openvrpaths.vrpath`. Selecting the
  existing standard user registry fixed initialization; code now does so only
  when no explicit override is supplied.
- Registration initially returned API success but did not install the app.
  Frame's manifest parser requires **`binary_path_linux_arm`**. Adding it fixed
  registration. Code now verifies `IsApplicationInstalled`, not return status alone.
- Repeated Add, Remove and re-Add of **only** `local.frameyap.overlay` succeeded;
  autolaunch queried **off**. The final installed panel check also succeeded after
  explicit identification with that registered key. Menu-driven launch and cold
  SteamVR startup were not exercised.

## Installer and final state

Native-only artifacts `v0.1.0-poc1`/`poc2` exercised installation, same-version
rerun, upgrade, rollback and forward selection. A producer-prefix SDL RUNPATH leak
was found and removed; final ELF RUNPATH is only `$ORIGIN/../lib` and bundled
SDL/OpenVR resolve inside the installed tree.

Explicit OpenVR unregister followed by `--uninstall --unregistered` succeeded.
The final `v0.1.0-poc3` artifact was then installed twice and registered with
autolaunch off. It includes native code, SDL/OpenVR, Hack font/notices and the
worker adapter, **no ASR runtime or weights**. Installed size: ~8.1 MiB.

Local archive SHA-256:
`90aab107feea2cb501bf815806bdc1d04d84bb6f87f7c795b60ad43c7fdc0634`.
The artifact is retained only in the device's project-owned development directory;
no GitHub release/upload was performed. Installed launcher is `~/.local/bin/frameyap`.
Actual native CLI and installer both refused an independently held install lock.
At the final check, no owned `frameyap` process or transient `frameyap-*` runtime
directory remained. Installation/registration remain; nothing was autostarted.

Final verification on source `d60d8d2`: default local build **8/8 CTests**;
optional native local build **9/9**; native ARM64 build **9/9**. Suites include
13 installer cases, worker framing/hash/cancellation/deadlines, gestures, state,
instance locking, and an isolated fake Wayland server. Those tests do not connect
to the live compositor or record audio. `git diff --check` passed locally.

## Still open

Runtime permission and any public distribution; live microphone quality/device
selection; physical tap/hold/buttons; actual text/Enter delivery into disposable
and real target apps; focus/held-modifier behavior; quick typing's focus observer;
scene/dashboard collisions; compositor cost, battery/thermal and headset comfort.
No live microphone recording or input injection occurred in this POC pass.
