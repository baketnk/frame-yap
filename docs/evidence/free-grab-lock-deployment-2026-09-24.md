# Free controller grab / layout lock deployment — 2026-09-24

Historical deployment evidence; physical interaction remains user-led acceptance.
The user authorized continuing after the [raw-overlay incident analysis](raw-overlay-teardown-crash-2026-09-24.md), using controller/visual testing instead of another raw-overlay probe.

- Source: `0d4cfbfdb74aaaa8e56be844122ac51eeb8fe365`. Includes full controller-relative
  position/rotation, saved `lock_layout`, corrected top-left mask coordinates, and
  earlier gradient, clock/date and wrist-fade commits.
- Local default suite: 15/15 passed. Local native suite: 20/20 passed.
  Native ARM64 Release build on Frame: 20/20 hardware-free tests passed.
- Exact source archive SHA-256 (verified before extraction):
  `57aa17045ddfee0fc8f6f1bb2bd4cbf19b082cf62321edaded2fb40ebd19e9d2`.
- Installed version: `2026-09-24T210207Z-g0d4cfbfd`.
  Native-only external-runtime package SHA-256:
  `85574b1645b68e57a609c3e46b8068613ac5bc64a25fbc8b7ed7c1e4bcb17f9a`.
  Installed/staged binary SHA-256 both matched:
  `4d256feb1b23b649e4720fb3ca11eab786229a445c22d4fc31b209ffe7e6b187`.
- No existing FrameYap process was running at install. The managed installer
  retained rollback and backed up the existing config before filling missing
  preferences. No ASR runtime/model was bundled or downloaded.
- Installed `--version` and the running executable path were verified after
  launching the normal Vulkan runtime (PID 78418 at that check). Startup reported
  dashboard visible, normal input priority, Lasers anytime off and panel-shown=N.
  The latter is not visual acceptance; the selected wrist's angle/tracking gate
  can keep it hidden. The user was asked to face the wrist toward them if needed.
- No raw-overlay probe, deliberate recording or synthesized input was run during
  this deployment. No SteamVR, Gamescope, SSH or terminal session was restarted.

Requested human checks: corner-bracket hit/resize; free depth/rotation and stable
release; Settings lock hides/disables both handles and unlock restores them.
At this record's creation those checks were requested, not yet reported passed.
