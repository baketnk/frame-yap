# Frame Dictation development

This is an independent project, not a plugin for another application.
Read README.md and docs/design.md before implementation. Dated evidence records
past observations; it never proves current device availability or grants a live run.

- Keep dependencies explicit and small. Do not add a dependency/submodule/symlink
  to an unrelated application's build tree, assets or Python environment.
- External source reuse needs a deliberate, license-reviewed standalone extraction,
  not hidden coupling.
- Preserve the one-command GitHub installation goal: no Steam store AppID, sudo
  or end-user compiler requirement. See docs/install-design.md. Do not publish a
  placeholder installer as functional or conflate an OpenVR app key with a store ID.
- Default builds and tests are offline and hardware-free. No implicit package/model
  downloads, microphone recording, input injection or OpenVR initialization.
- Hardware tests must be opt-in and distinguish API discovery from delivered input,
  transcription quality, performance and human headset acceptance.
- Preserve existing SSH and user sessions; never terminate SSH/session processes or
  use broad cleanup/restart commands. Stop only processes this project owns.
- Keep audio, transcripts, model weights, credentials and private logs out of Git.
  No automatic Enter/submit, speech commands or cloud/desktop ASR fallback.
- Build/check: `cmake -S . -B build && cmake --build build`, then
  `ctest --test-dir build --output-on-failure` and `git diff --check`.
- Keep docs honest about implemented versus proposed behavior. Review exact diffs
  and create small verified commits.
