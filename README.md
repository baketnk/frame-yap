# FrameYap

Voice typing on Steam Frame, with recognition on the headset rather than a desktop or cloud server.
Hold a controller button to record, review the transcript, then deliberately type it into the focused app.
Standalone OpenVR overlay; no Steam store AppID or sudo. First v0.1 release is in progress.

## Requirements

- Steam Frame with usable SteamVR/OpenVR and Gamescope for the **native** overlay and text delivery; Linux ARM64/glibc for the current installer payload format.
- For voice recognition, separately provision a compatible **CPU Python runtime** (moondream 2.4.0 / Kestrel 0.8.0 and dependencies) and the pinned local Parakeet Redux model. Neither is bundled or installed with pip by the current native-only installer. There is no fallback ASR service.
- A local source build needs CMake 3.20+, C++20 and explicit native libraries/SDK; the default hardware-free build needs only CMake and C++20. See [build requirements](docs/build.md).

## Install (local artifacts only)

**No public release is published.** To install a locally vetted, checksummed ARM64 native-only archive without a compiler:

```sh
sh install.sh --mode binary --archive /path/to/frameyap-VERSION-linux-aarch64.tar.gz \
  --sha256 ARCHIVE_SHA256 --version VERSION
```

`VERSION` is numeric `0.1.YYYYMMDDHHMM` for this release line; a future release tag will be `vVERSION`. The installer has an explicit local source-build path with toolchain/dependency inputs, and a read-only `--print-plan`/machine-readable `--json` mode; the producer stage→archive path still needs a clean-account artifact test and release audit. See [packaging](docs/packaging.md). Installation is user-local with rollback, no automatic launch or registration, and no Steam store AppID. Before voice typing, independently supply a licensed Python CPU environment and pinned weights, and configure their absolute paths in `~/.config/frameyap/paths.conf` (or the XDG config equivalent). Model download is opt-in and separate: `sh install.sh --install-model --backend redux --print-plan --json` inspects pinned metadata, while `--yes` explicitly authorizes installation from the *installed* manifest; `--expected-manifest-sha256 HASH` additionally binds consent to its exact bytes. The locally implemented Settings → Models chooser offers selection/restart and a two-click Install/Confirm Install with source, size, license, attribution and manifest SHA-256; it has not been validated as an installed/headset flow. No chooser action installs the Python runtime. Do not mistake a native-only install for working ASR. The pinned GitHub download route must not be advertised as functional until a vetted release exists.

## Controls (default Frame binding)

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

Controls are remappable in SteamVR. Grip gestures may be unavailable with the dashboard open; pointer controls are an alternative. At the 4096-byte transcript limit, Type preserves the full text without appending a space if none fits. No speech commands, automatic Enter or automatic submit. Review is the default; Settings → Auto insert is opt-in, normally text + space only under continuously observed Xwayland focus. Type and Type + Enter now require a verified Xwayland target and use paced direct typing; Cancel stops remaining batches, never undoing prior input. The clipboard stays untouched. Check the destination before typing. Edit literal Quick phrases (`quick_inputs`) in `$XDG_CONFIG_HOME/frameyap/config.json`, then restart. See [overlay and settings](docs/overlay.md).

## Status / not yet validated

Native ARM64 build, CPU inference on a public clip, overlay visibility, Gamescope API discovery, a controls-only Right X probe and native-only installation have been exercised on Frame. **Live microphone → reviewed transcript → real target delivery has not been formally accepted.** Focus guard and insertion have offline/owned-target checks, not general app compatibility or human headset acceptance. Global bindings, physical gesture feel, Auto insert with speech, latency, battery/thermal cost and controller coexistence still require opt-in headset testing. Front-prefix loss on repeated submissions (P1) remains under separate investigation; do not treat it as fixed. An old-code fixture crashed the Gamescope session, not the OS, and does not prove the new delivery path. Local code/build status does not mean the device was updated. A completed Gamescope IME call means *input queued*, not that an app consumed or submitted it.

The native app normally keeps the mic device open while Ready and discards idle audio; it never mutes other applications' microphones. Settings → **Close mic when idle** (default OFF) closes it between clips, but reopening on PTT can cause an audio spike, delay or first-syllable clipping. **Lasers anytime** (default OFF) requests system-wide lasers while the panel is visible, potentially affecting games; it is not SteamVR's experimental input override. Review, settings, placement and diagnostic details: [overlay](docs/overlay.md), [worker](docs/worker.md), [design](docs/design.md).

Version output is numeric `frameyap MAJOR.MINOR.YYYYMMDDHHMM` (currently `0.1`); a development build may print `git HASH` and optionally `(uncommitted changes)` on a **separate** line. The UTC timestamp is set at configuration time (`SOURCE_DATE_EPOCH` can supply it); release archives must be built from a clean `v0.1.<timestamp>` tag. Offline model inventory and pinned SHA-256 checks in a source-tree build: `./build/frameyap --list-models`, `./build/frameyap --check-model redux --model-dir /absolute/model`, or `python3 scripts/model-status.py` with the same options. Packaging retains the verifier script for installed CLI use, which still needs a clean-account artifact check. Native `--run` accepts `--backend ID`, `--model-store /absolute/dir`, `--manifest-dir /absolute/dir` overrides; the installed launcher passes explicit flags through to the binary. These locally wired paths do not imply an installed release was tested or a model/runtime was supplied. No model or runtime is downloaded by status checks or on normal launch. [Dependency licenses and outstanding release audit](docs/third-party.md); [TODO](TODO.md).

No recordings, transcripts, private logs, weights or CPU runtime binaries are committed. No default build/test downloads or initializes SteamVR, microphone or input injection.
