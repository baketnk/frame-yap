# Debug diagnostics and right-wrist correction — 2026-09-24

Historical observation, not current availability or permission to run tests.

The wearer clarified that the right-wrist panel faced away (its back was visible).
Commit `29f3729d` reverses right-wrist panel-right and panel-front, preserving
panel-up, center and size. Offline pose tests check both hands, orthonormality
and positive determinant over several rolls; this is not physical acceptance.

The wearer also reported repeated generic transcription failures. The retained
app log contained startup status only. Inspection found that the Python worker
replaced every request exception with `transcription failed`, and both the native
parent and Python suppressed stderr. No underlying exception had been retained;
the root cause therefore remained unknown. The user declined a public-clip
inference trial and chose to retry speech themselves after diagnostics were added.

Commit `6592e1af` adds default-safe stage/category errors and explicitly opt-in
advanced debugging through config/Settings. Full worker stdout/stderr, tracebacks
and transcripts may appear in private bounded logs when enabled; no raw clip
archive is created. The native receiver allowlists error labels before showing
or logging the non-debug error. Settings changes restart the owned worker and
discard current work; defaults remain off. See [diagnostic policy](../worker.md#advanced-debugging).

## Checks and deployment

- Local default CTest: 13/13; local native hardware-free CTest: 16/16.
- Inspected a synthetic Settings canvas containing the toggle and privacy warning.
- ARM64 native build from Git archive `6592e1af`: hardware-free CTest 16/16.
  Includes fake-child stderr/protocol isolation, log bounds/rotation/permissions,
  unsafe path refusal, opt-in/off behavior and safe-error privacy tests.
- Installed version: `2026-09-24T190806Z-g6592e1af`.
- Native-only archive SHA-256:
  `eac25ae6e36970301e5cb67614eaa4053d20b79711397b462e2caf1359c09709`.
- Checksum-verified installer selected the version above; installed `--version`
  and the restarted process's executable path matched. Previous version retained:
  `2026-09-24T184918Z-g7a3a5451`. The intermediate wrist-only archive was not installed.
- Runtime/model paths configuration hash unchanged. Installer backed up the
  previous config, added `advanced_debug: false`, and retained normal priority,
  approved X/B/A/Y mappings and the wrist size/offset settings.
- The saved mount was now `left-wrist` (changed since the earlier right-wrist
  observation); installation preserved it rather than choosing for the wearer.
- No FrameYap process was present immediately before this install. Relaunched
  only FrameYap. No SteamVR/SSH/user-session process was stopped. Startup reported
  normal priority and Lasers anytime off; this does not establish panel visibility.

No public-fixture inference, assistant-triggered microphone recording or input
injection was performed. Normal authorized app startup warms the configured worker
and opens/discards idle microphone samples. Advanced logging was left **off** for
the wearer to enable. Actual failure diagnosis, Settings-toggle behavior on Frame,
and physical wrist acceptance still require the user's next trial.
