# FrameYap TODO

Compiled from the 2026-09-24 holistic review, with owner decisions applied (see
"Decisions" at the end). Items are sized to hand off individually. Size: S ≈ hours,
M ≈ a day, L ≈ multi-day.

Guardrails from `AGENTS.md` apply to every item: offline default build, no implicit
downloads, no automatic Enter, small verified commits, docs must state implemented
vs proposed behavior honestly.

## A. First release (v0.1) blockers

- [x] **A1. Remove the runtime-license blocker notes.** Upstream's Kestrel README
  states local inference is free; blocker text removed from README/docs/scripts and
  replaced with a neutral dependency note in `docs/third-party.md`.
- [x] **A2. Remove references to the unrelated app (kouseki).** (S)
  Docs and one `src/panel_surface.cpp` comment were cleaned; remaining check is a
  final grep. Keep the Inconsolata/OFL attribution and cite the
  upstream font source (googlefonts/Inconsolata) instead.
  *Done when:* `grep -ri kouseki` is empty and the font SHA/attribution remains.
- [ ] **A3. Commit the pending `AGENTS.md` rename** ("Frame Dictation" →
  "FrameYap"). (S)
- [ ] **A4. Inventory the remaining runtime dependencies' licenses.** (M)
  Torch CPU, numpy, tokenizers, SDL3, wayland, libxcb, FreeType (pick FTL or GPL
  option), compiler runtime / libc floor. Prerequisite for shipping a prebuilt
  archive that includes any of them.
- [ ] **A5. Drop "POC" from the shipped surface.** (S) `--help` text, README,
  `scripts/stage-native-poc.py`, `CMakeLists.txt` messages,
  installer strings. v0.1 is a first small release.
- [ ] **A6. Rewrite the README front.** (M) 3-line pitch, requirements, install,
  controls **table** (button → action), then a "Status / not yet validated" section.
  Today it reads as a lab notebook and Controls is a wall of text.
- [x] **A7. Archive docs.** Dated evidence (`docs/evidence/`) and `provenance.md` moved
  to the untracked, gitignored `docs/archive/`; `poc.md` renamed `docs/build.md`.
  Remaining: skim `design.md`/`overlay.md`/`packaging.md` for stale "proposal" and
  hedging language before v0.1.

## B. Correctness / robustness

- [ ] **B1. A malformed transcript must not kill the worker.** (S)
  `src/runtime.cpp:141` → `session.reply()` → `literal_text()` throws on control
  characters or bad UTF-8; the catch at ~line 167 calls `worker.stop()` and
  `session.fail()`, unloading the model (reload can take up to 120 s). Treat it as a
  request-level error (like the `E` path: keep the worker, show "transcription
  failed", allow retry). Add a test with a control-character reply asserting the
  worker stays ready.
- [ ] **B2. Make the C++ side engine-agnostic.** (S) `src/worker.cpp` hardcodes
  "moondream/torch" in the user-facing `F`/`I` errors. Use neutral wording or a
  worker-supplied message code. Prerequisite for C1.
- [ ] **B3. "Close mic when idle" setting, default OFF.** (M)
  Default keeps the mic open while Ready (opening/closing per PTT causes an audio
  spike on the physical hardware, and it avoids first-syllable clipping). The
  setting closes it between clips for people who don't want a live device. Document
  the tradeoff (spike/latency) next to the toggle, in Settings and in the docs.
  Persist in `config.json`; add to the panel Settings tab.
- [ ] **B4. Extract the interaction logic from `run()` and test it.** (L)
  `run()` in `src/runtime.cpp` is one ~220-line function of captured lambdas with no
  tests. Pull out a `Controller` (events + worker/audio/input interfaces → `Panel`)
  so PTT, cancel, quick phrases, auto-insert and error transitions are testable
  without hardware. B1 is the first regression test.

## C. Backends and model management

- [ ] **C1. Multiple ASR backends behind the worker protocol.** (L)
  Keep Redux as the default, allow additional backends (whisper.cpp, faster-whisper,
  sherpa-onnx Parakeet, …) as separate worker executables speaking the existing
  `Y`/`T`/`R`/`E` framing. Define a small backend manifest (id, display name,
  launcher, pinned model files + hashes + attribution, license text, CPU/GPU
  requirements) so nothing is hardcoded in C++ or `model_files.py`.
  *Done when:* Redux is expressed as a manifest, and a second backend can be added
  without touching `worker.cpp`/`runtime.cpp`.
- [ ] **C2. Model/backend state and a chooser in the UI.** (L) Depends on C1 + B4.
  Settings page listing backends/models with state (not installed / installed and
  verified / loading / ready / failed), the active one marked, and selection that
  restarts the worker. Target user is non-technical: an **Install** button on the
  panel runs the installer's machine-readable mode (D2) as a child process and shows
  progress/errors in the panel, so nobody needs a terminal. The click is the consent;
  there are still no implicit or background downloads, and the panel states what
  will be downloaded and how large it is before it starts. Persist the choice in
  `config.json`.
- [ ] **C3. Model status CLI.** (S) `frameyap --list-models` / `--check-model ID`
  (offline, hash-verifies installed files) so the UI and installer share one
  implementation.

## D. Installer

- [ ] **D1. Installer with a binary-or-source choice.** (L)
  `install.sh` offers *prebuilt archive* (checksummed) or *build from source*
  (checks toolchain/deps via `install-preflight.sh`, builds in a private dir), then
  continues automatically through install after the user's choices. Retain rollback,
  idempotency, no sudo, no Steam AppID, opt-in autolaunch.
- [ ] **D2. Model-agnostic, attended-or-unattended operation.** (M)
  Every prompt has a flag (`--mode binary|source`, `--backend ID`, `--model-dir`,
  `--yes`, `--autolaunch`/`--no-autolaunch`, `--without-model`, `--print-plan`,
  `--json` output) so a model/agent can run it non-interactively; interactive
  prompts only run on a TTY and print the equivalent flags they chose. Exit codes
  and messages must be machine-readable.
- [ ] **D3. Publish a first prebuilt ARM64 archive.** (M) Depends on A4. Follow the
  release checklist in `docs/packaging.md`; do not advertise the one-command route
  until the archive and its checksum are actually published and tested from a clean
  account.

## E. Naming and versioning

- [x] **E1. Name: keep "FrameYap" for v0.1.** Frame (the hardware) + yap (speech)
  says what it is; no rename churn before the first tag. If a hardware-neutral
  project name is wanted later (e.g. plain "Yap", with FrameYap as the Steam Frame
  front end), decide it before the cross-window work in G, not now.
- [ ] **E2. Rename/explain UI terms.** (S) Delivery actions become **Type** (text +
  space) and **Type + Enter**; align overlay buttons, `--check-controls` output,
  README, help text and `docs/overlay.md`, and keep `UiAction::Enter` internal only
  if labels are consistent. "Quick chat" → "Quick phrases". Add one-line Settings
  explanations for "Hold Quit" and "Lasers anytime". Explain the "Parakeet Redux"
  vs `moondream` naming once in `docs/worker.md`.
- [ ] **E3. Version scheme `MAJOR.MINOR.YYYYMMDDHHMM`.** (S)
  e.g. `0.1.202609241530`: valid semver (numeric patch, no leading zeros), sorts
  correctly, keeps the build date visible, URL/filename-safe. The version field is
  just that string. The old `-gHASH` (which commit) and `-dirty` (uncommitted
  changes) suffixes were only for telling developer builds apart, so they move out
  of the version: `--version` prints `frameyap 0.1.202609241530`, and for a dev
  build adds a second line like `git abc12345 (uncommitted changes)`. Release
  archives are built from a clean tag, so users never see it. Update the CMake
  version regex/`FRAMEYAP_VERSION`, `tests/cli.cmake`, `package-release.py`,
  installer version checks and docs. `SOURCE_DATE_EPOCH` still drives the
  timestamp. Tag releases `v0.1.<timestamp>`.

## F. Code structure (non-urgent)

- [ ] **F1. Move `--check-*` diagnostics out of `main.cpp`** (S) — ~70 lines of
  inline UI plus hand-rolled per-mode argument checks; use a `check.cpp` and a
  table-driven option parser.
- [ ] **F2. Split `overlay.cpp`'s `Impl`** (M) — ~40 loosely related members (drag
  state, save-failure flags, counters, pose caches): separate drag, persistence and
  diagnostics.

## G. Other windows (post-release)

Text delivery already works into a WezTerm window on Frame (owner-tested; this is
not the recorded live acceptance in H). Deeper integration of other windows with
this app is future design and out of scope for v0.1.

- [ ] **G1. Validate browser text fields via the Gamescope input path.** (M)
  Highest-priority target. Record which fields accept Type / Type + Enter (plain
  inputs, textareas, rich editors, password fields should be expected to differ) and
  document the results honestly.
- [ ] **G2. Later, if needed:** a KDE desktop-mode backend behind `DeliveryLease`,
  and a uinput backend as a last resort. Not scheduled; uinput needs `/dev/uinput`
  access and types with no focus check, which conflicts with the project's
  no-sudo/udev rule and per-window authorization.

## H. Carried over from the earlier TODO (hardware validation)

- [x] Basic menu launch on Frame: the user found FrameYap in Steam's **Non-Steam**
  section; the panel showed and Quit worked. This does not validate transcription,
  recording, text delivery, cold startup, or which shortcut discovery mechanism Steam
  used.
- [ ] Verify the exact missing-runtime panel message and Steam's shortcut persistence
  across a normal restart (without restarting sessions just for the test). Confirm no
  runtime/model environment override before future live checks.
- [ ] Live acceptance on Frame: microphone → reviewed text → real target delivery,
  Auto Insert with speech, physical resize.

---

## Decisions

- Redux runtime: treated as usable for local inference per upstream's Kestrel
  README; not bundled in our archives (A1 done).
- Multiple backends + a model chooser UI: wanted (C1–C3). The panel can trigger the
  install on an explicit click, for non-technical users.
- Name: keep FrameYap. v0.1 is a first small release, not a POC.
- Close-mic-when-idle: setting, default off (B3).
- Labels: Type / Type + Enter (E2).
- Installer: binary or source, continues automatically after choices, fully flag-
  driven for agent use (D1–D2).
- Version: `MAJOR.MINOR.YYYYMMDDHHMM`, git hash only in dev-build `--version` output
  (E3).
- Other windows: browser text fields first (G1); deeper integration is future design.

## Open questions

None currently blocking.
