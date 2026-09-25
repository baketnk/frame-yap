# Development notes

Technical details moved out of the user-facing README. See also
[build](build.md), [overlay](overlay.md), [worker](worker.md),
[packaging](packaging.md), [install design](install-design.md),
[design](design.md) and [third-party notes](third-party.md).

## Requirements (detail)

- Steam Frame with usable SteamVR/OpenVR and Gamescope for the **native** overlay and text delivery; Linux ARM64/glibc for the current installer payload format.
- For voice recognition, a **CPU Python runtime** (moondream 2.4.0 / Kestrel 0.8.0, CPU Torch 2.8.0) and the pinned local Parakeet Redux model. Neither is bundled; each has its own explicit install step: `sh install.sh --install-model --backend redux --yes` and `sh install.sh --install-runtime --yes` (needs Python 3.10–3.13 with venv; about 200 MB download). Use `--print-plan` first to see exactly what each fetches. There is no fallback ASR service.
- A local source build needs CMake 3.20+, C++20 and explicit native libraries/SDK; the default hardware-free build needs only CMake and C++20. See [build requirements](build.md).

## Controls (full semantics)

| Button / control | Action |
| --- | --- |
| Right X, hold / release | Record while held; release to transcribe. |
| Right B | Cancel/discard, or close the Quick phrases picker. |
| Right A | **Type:** queue reviewed text, normally with a trailing space. With nothing to review, a press queues Enter alone (so a quick double press types then submits). |
| Right Y | Open **Quick phrases**; press again to cycle the selection. |
| Left grip, double-tap | **Type + Enter:** queue the selected phrase verbatim + Enter, pending review (normally + space) then Enter, or Enter alone if neither exists. |
| Overlay Record / Stop | Click-to-start/stop alternative to the PTT binding. |
| Overlay Type / Type + Enter | The same deliberate text / Enter actions. |
| Overlay Hold Quit | Hold for 0.9 seconds, then release to quit (prevents accidental exit). |

Controls are remappable in SteamVR. Grip gestures may be unavailable with the dashboard open; pointer controls are an alternative. At the 4096-byte transcript limit, Type preserves the full text without appending a space if none fits. No speech commands, automatic Enter or automatic submit. Review is the default; Settings → Auto insert is opt-in, normally text + space only under continuously observed Xwayland focus. Type and Type + Enter now require a verified Xwayland target and use paced direct typing; Cancel stops remaining batches, never undoing prior input. The clipboard stays untouched. Check the destination before typing. Edit literal Quick phrases (`quick_inputs`) in `$XDG_CONFIG_HOME/frameyap/config.json`, then restart. See [overlay and settings](overlay.md).

## Validation status

Native ARM64 build, CPU inference, overlay visibility, Gamescope API discovery and native-only installation have been exercised on Frame. The owner uses live microphone → reviewed transcript → paced delivery into real apps day to day, and the earlier front-prefix loss on repeated submissions (P1) is no longer observed. General app compatibility (for example browser fields), Auto insert with speech, physical resize, and battery/thermal cost are not yet validated. On 2026-09-25 the owner removed the previous install, ran the published v0.1.202609251524 `curl | sh` installer on the Frame (FrameYap, model and pip runtime) and voice-typed successfully; the ARM64 runtime install is therefore exercised, but the missing-runtime panel message and battery readouts on Frame were not separately checked. Local code/build status does not mean the device was updated. A completed Gamescope IME call means *input queued*, not that an app consumed or submitted it.

The native app normally keeps the mic device open while Ready and discards idle audio; it never mutes other applications' microphones. Settings → **Close mic when idle** (default OFF) closes it between clips, but reopening on PTT can cause an audio spike, delay or first-syllable clipping. **Lasers anytime** (default OFF) requests system-wide lasers while the panel is visible, potentially affecting games; it is not SteamVR's experimental input override. Review, settings, placement and diagnostic details: [overlay](overlay.md), [worker](worker.md), [design](design.md).

Version output is numeric `frameyap MAJOR.MINOR.YYYYMMDDHHMM` (currently `0.1`); a development build may print `git HASH` and optionally `(uncommitted changes)` on a **separate** line. The UTC timestamp is set at configuration time (`SOURCE_DATE_EPOCH` can supply it); release archives must be built from a clean `v0.1.<timestamp>` tag. Offline model inventory and pinned SHA-256 checks in a source-tree build: `./build/frameyap --list-models`, `./build/frameyap --check-model redux --model-dir /absolute/model`, or `python3 scripts/model-status.py` with the same options. Packaging retains the verifier script for installed CLI use, which still needs a clean-account artifact check. Native `--run` accepts `--backend ID`, `--model-store /absolute/dir`, `--manifest-dir /absolute/dir` overrides; the installed launcher passes explicit flags through to the binary. These locally wired paths do not imply an installed release was tested or a model/runtime was supplied. No model or runtime is downloaded by status checks or on normal launch. [Dependency licenses and outstanding release audit](third-party.md); [TODO](../TODO.md).

No recordings, transcripts, private logs, weights or CPU runtime binaries are committed. No default build/test downloads or initializes SteamVR, microphone or input injection.

## Release builds

Release archives are built on Linux ARM64 from a clean `v0.1.<timestamp>` tag
with `sh scripts/build-release.sh WORKDIR VERSION`, which fetches SHA-256-pinned
SDL3 and OpenVR SDK sources, builds, tests, stages and packages. The release
commit sets `RELEASE_VERSION` in `scripts/install_payload.py` (then
`python3 scripts/sync-installer.py`); upload the archive, its `.sha256` and
`install.sh` to the GitHub release.
