# FrameYap TODO

Compiled from the 2026-09-24 holistic review, with owner decisions applied (see
"Decisions" at the end). Items are sized to hand off individually. Size: S ≈ hours,
M ≈ a day, L ≈ multi-day.

Guardrails from `AGENTS.md` apply to every item: offline default build, no implicit
downloads, no automatic Enter, small verified commits, docs must state implemented
vs proposed behavior honestly. Checked source-work items below mean the local
implementation is present, **not** a shipped or headset-accepted release.

## Implemented locally; offline tested; installed/headset validation pending

The current working tree includes the source changes for A3, A5–A7, B1–B4,
C1–C3, D1–D2, E2–E3 and F1–F2. Final offline checks passed 24/24 default,
25/25 strict UI, and the fake Wayland input regression suite. An earlier private
native ARM64 snapshot passed 28/28; the final source requires a fresh native
build before deployment.
These checkboxes close the *source tasks*, not their empirical acceptance gates.
C1's second backend is a fake executable fixture, **not** a second shipped ASR
engine. C2's two-click model consent, SHA-bound installer handoff, and D1/D2's
source/attended installer paths still need an audited native archive and an
installed clean-account/Frame exercise. A4 (exact artifact ABI/license closure),
D3 (publication and clean-account acceptance), P1 (real-target delivery), G1
(browser), and H (live headset acceptance) remain open. The 0.1.202609250333 build is installed on Frame but not launched or
accepted; local code/tests cannot establish a fixed delivery regression.

## Priority regression (reported during implementation)

- [ ] **P1. Repeated delivery loses the beginning of later submissions.** After
  the first Type + Enter, later text reportedly loses a dozen to a few dozen leading
  bytes. Investigate preview versus destination loss, retain the full bounded
  literal transcript, and add repeated/long/Unicode delivery regression tests.
  Do not assume a larger buffer fixes it or retry uncertain delivery automatically.
  An old-code delivery fixture crashed the Gamescope session, **not** the OS;
  this is not evidence of a fix. Plain-ASCII prefix corruption was reproduced;
  retained IME and immediate byte chunking did not fix it. A nonblocking,
  focus-guarded 24-codepoint/150 ms delivery queue is implemented with offline
  regressions, including full-length Unicode and all-space batches. Clipboard
  remains untouched. Real-target confirmation is still required after deployment.
  Device update (2026-09-24): version 0.1.202609250333 was installed on Frame
  (version and binary hash checked; not launched; runtime paths preserved; config
  migration backed up); the deployment peer reported 29/29 native tests. Synthetic
  repeated text and a full 4,096-byte payload arrived exactly. A later mismatch
  looks consistent with receiver keymap caching, not Unicode trimming or space
  substitution, but that is unconfirmed. P1 stays open until real-app delivery is
  verified; this is not headset acceptance.

## A. First release (v0.1) blockers

- [x] **A1. Remove the runtime-license blocker notes.** Upstream's Kestrel README
  states local inference is free; blocker text removed from README/docs/scripts and
  replaced with a neutral dependency note in `docs/third-party.md`.
- [x] **A2. Remove references to the unrelated app.** (S)
  Tracked references were removed while preserving Inconsolata/OFL attribution,
  font SHA-256 and upstream googlefonts/Inconsolata source. A tracked,
  case-insensitive grep for the former app name must remain empty.
- [x] **A3. Commit the pending `AGENTS.md` rename** ("Frame Dictation" →
  "FrameYap"). (S) Done in baseline checkpoint `07c03ea`.
- [ ] **A4. Inventory the remaining runtime dependencies' licenses.** (M)
  Upstream inventory and the FreeType FTL choice are documented, but the exact
  staged ARM64 native/runtime binaries, transitive wheel/library notices, symbol
  versions, loader and libc floor still require artifact-specific review before
  publishing any prebuilt archive.
- [x] **A5. Drop "POC" from the shipped surface.** (S) Public help, README,
  CMake and installer use the release name. `scripts/stage-native-poc.py` remains
  a deprecated compatibility wrapper for `scripts/stage-native.py`, not the
  documented or shipped staging entry point. v0.1 is a first small release.
- [x] **A6. Rewrite the README front.** (M) Pitch, requirements, local-only install,
  controls table and explicit "Status / not yet validated" section are present.
- [x] **A7. Archive docs.** Dated evidence (`docs/evidence/`) and `provenance.md` moved
  to the untracked, gitignored `docs/archive/`; `poc.md` renamed `docs/build.md`.
  Current design/overlay/packaging docs distinguish implemented local behavior
  from proposed and unaccepted headset/release behavior.

## B. Correctness / robustness

- [x] **B1. Keep malformed transcripts request-local.** (S)
  `Controller::tick()` catches a correlated bad UTF-8/control reply and fails the
  request without stopping the ready worker; hardware-free fakes test retry.
- [x] **B2. Use engine-neutral C++ worker errors.** (S)
  `F`/`I` messages no longer name Redux's Python engine.
- [x] **B3. "Close mic when idle" setting, default OFF.** (M)
  Config/Settings and fake-backed mic-lifetime tests cover default idle draining
  versus opt-in close/reopen; the physical spike/latency tradeoff is documented.
- [x] **B4. Extract/test interaction logic from `run()`.** (L)
  `Controller` receives injectable audio/worker/focus/delivery interfaces; offline
  tests cover PTT, cancel, phrases, Auto insert and error/retry transitions.

## C. Backends and model management

- [x] **C1. Manifest-driven worker backends (source capability).** (L)
  Redux's pinned manifest and a generic local dispatcher implement the existing
  `Y`/`T`/`R`/`E` framing. An offline fake second executable backend works without
  edits to C++ worker/runtime; only Redux has a shipped inference engine. New
  backends still require license/runtime and actual inference validation.
- [x] **C2. Model/backend state and chooser (local source).** (L)
  Settings lists manifest-backed model status and active/loading/ready/failure
  states; selection persists and invalidates/restarts worker, clip and review.
  Install → Confirm Install displays pinned source, size, license, attribution and
  manifest SHA-256; confirmation launches an owned installer helper with that
  digest and offline rechecks afterward. Full consent metadata is paginated;
  the panel shows bounded per-file download/verification events and sanitized
  errors. Installer output alone never proves success. No implicit downloads or
  runtime install. **Installed chooser/consent behavior on Frame remains unvalidated.**
- [x] **C3. Offline model status CLI.** (S) `frameyap --list-models` and
  `--check-model ID` dispatch pinned local manifest/hash checks, without ASR or
  downloads. Installed archive CLI still needs clean-account verification.

## D. Installer

- [x] **D1. Local binary-or-source installer paths.** (L)
  `install.sh` has a checksummed binary route and explicit local source/toolchain
  preflight/build/stage/install route, with rollback and no default registration.
  A native archive was exercised in private HOME/XDG roots on Frame: install,
  installed model-status CLI, idempotency, wrapper repair and uninstall. This
  same-user/system-library fixture is not clean-account acceptance.
- [x] **D2. Attended-or-unattended model-agnostic installer interface.** (M)
  TTY choices have equivalent flags; `--mode binary|source`, `--backend ID`,
  `--model-dir`, `--yes`, `--autolaunch`/`--no-autolaunch`, `--without-model`,
  `--print-plan` and `--json` support offline plans and structured outcomes;
  explicit model installs use installed pinned manifests. Tested with local
  fixtures only, not a released archive or an installed Frame UI handoff.
- [ ] **D3. Publish a first prebuilt ARM64 archive.** (M) Depends on A4. Follow the
  release checklist in `docs/packaging.md`; do not advertise the one-command route
  until the archive and its checksum are actually published and tested from a clean
  account.

## E. Naming and versioning

- [x] **E1. Name: keep "FrameYap" for v0.1.** Frame (the hardware) + yap (speech)
  says what it is; no rename churn before the first tag. If a hardware-neutral
  project name is wanted later (e.g. plain "Yap", with FrameYap as the Steam Frame
  front end), decide it before the cross-window work in G, not now.
- [x] **E2. User-facing Type / Type + Enter / Quick phrases labels.** (S)
  Overlay, diagnostics, README/help and overlay docs align on these controls;
  `insert`, `enter`, `quick_chat` remain internal binding/API names. Settings
  explains Hold Quit and Lasers anytime; worker docs distinguish the Redux model
  from its `moondream` Python inference package.
- [x] **E3. Numeric `MAJOR.MINOR.YYYYMMDDHHMM` version source work.** (S)
  CMake, CLI/tests, package producer and installer use numeric release versions;
  dev git info is a separate `--version` line. `SOURCE_DATE_EPOCH` can supply
  the configuration timestamp. A clean release tag/archive still needs D3.

## F. Code structure (non-urgent)

- [x] **F1. Move `--check-*` diagnostics out of `main.cpp`.** (S)
  `src/check.cpp` owns native checks; `src/cli.cpp` provides the table-driven
  parser. Device behavior remains separately gated.
- [x] **F2. Split `overlay.cpp`'s `Impl`.** (M)
  Drag state, persistence/save failures and diagnostics now have separate grouped
  owners; native snapshot build and offline panel/drag tests cover the refactor.

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
  Auto insert with speech, physical resize and a verified deployed version.

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

Release artifact compatibility/license audit, publication, P1 real-target behavior
and live headset validation are unresolved gates, not implied by checked source tasks.
