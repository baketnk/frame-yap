# Raw-overlay probe → compositor/session crash — 2026-09-24

Incident analysis from read-only boot, journal, coredump metadata and SteamVR log
inspection after the user reported a headset reboot and loss of their terminal.
No repeat crash experiment was performed during analysis. This record does not
authorize a probe, restart or deployment; live reproduction can destroy the desktop
session even when the test overlay is hidden.

## Established timeline (UTC)

- Linux stayed on the same boot throughout; uptime was about seven hours when
  checked at 20:55. This was **not an OS reboot**.
- 20:50:24.955: the normal FrameYap process called `VR_Shutdown`, then disconnected.
  Its later absence explains the read-only probe's `FindOverlay` failure; that
  failure is not evidence that SteamVR had already crashed.
- 20:51:42.264: the final `mask-probe` process started as `VRApplication_Overlay`.
- 20:51:42.279: it connected to the existing compositor (PID 2316).
- 20:51:42.284: it called `VR_Shutdown`; compositor logs record its pipe disconnect.
- 20:51:42.357: SteamVR's crash reporter was already processing a compositor dump.
  Coredump metadata timestamps the crash at 20:51:42, signal **SIGBUS (7)**.
- 20:51:42.563: systemd-coredump began processing it; completion was logged at
  20:51:44.195. The completion time is not the initial fault time.
- 20:51:47: `steamvr.service` failed and scheduled a restart.
- 20:51:57: Gamescope failed to stop within ten seconds; systemd killed its
  process group, including Xwayland, and restarted the graphical session.
  This accounts for the headset appearing to reboot and terminal-session loss.

Read-only `systemctl --user show` established the coupling:
`steamvr.service` has `Restart=always` and requires `gamescope-session.service`;
`gamescope-session.service` has `PartOf=steamvr.service graphical-session.target`
and a ten-second stop timeout. The agent did not issue a system reboot or kill
SSH/terminal processes; the service recovery cascade performed the session kills.

## Trigger sequence and result

The temporary native ARM64 probe used OpenVR SDK 2.15.6; client startup reported
runtime 2.17.10. It created its own hidden overlay, set an absolute pose, 1 m width,
mouse input and 1080×780 mouse scale, then:

1. Allocated a zero-filled 1080×780×4-byte RGBA buffer (3,369,600 bytes).
2. Called `SetOverlayRaw`; returned `VROverlayError_None`.
3. Applied one rectangular intersection mask, first at (996,32,68,68), then at
   (996,680,68,68). Both calls returned success.
4. Called `ComputeOverlayIntersection` for three synthetic rays per mask. All
   six returned false, including the intended positive cases. Therefore this
   probe **did not validate either mask coordinate convention**.
5. Immediately called `DestroyOverlay` and `VR_Shutdown`, without waiting for an
   image-loaded event or compositor upload completion. It never showed the overlay.

The normal FrameYap renderer uses a persistent Vulkan texture, not this raw-upload
path. Free-grab/lock changes were still local and were not running on the device.
The final probe source was retained at `/tmp/frameyap-mask-probe.cpp` locally and
`~/frameyap-poc/mask-probe.cpp` on Frame at analysis time; do not rerun casually.

## Crash evidence and interpretation

The faulting compositor thread's available stack began:

```text
__memcpy_sve                  libc.so.6 + 0xa0008
vrcompositor                 + 0x298cb8
vrcompositor                 + 0x6eaf4
vrcompositor                 + 0x11bcf0
vrcompositor                 + 0x11c0c4
vrcompositor                 + 0x14b9b4
vrcompositor                 + 0x14f9e0
vrcompositor                 + 0x183bf0
start_thread
```

The compositor core exists but was inaccessible to the unprivileged account;
no privilege escalation or core extraction was attempted. No GPU reset, kernel
panic or OOM event appeared in the inspected kernel interval 20:50–20:52.
SteamVR's own crash reporter automatically uploaded its minidump, reporting
CrashID `bp-f1d750cf-8943-4834-8873-9eec02260924`; the agent did not initiate that
upload or send a dump separately.

**Strongly supported trigger:** the hidden raw-overlay probe and its immediate
teardown. The compositor fault followed its disconnect within the same second.

**Leading unproven mechanism:** an asynchronous raw-image copy outliving its
shared-memory backing during overlay destruction/client shutdown. SIGBUS in
`memcpy` is consistent with an invalid/truncated mapped backing object. It does
not prove that mechanism: raw-upload buffer sizing/limits or another compositor
memory-handling fault remain alternatives. Symbols, fault-address/mapping data,
or a controlled comparison are needed to distinguish them.

## Consequences for further work

- Hidden overlays are not isolated from compositor upload/lifetime machinery.
- API success and a clean probe exit do not establish compositor completion.
- Keep normal rendering on the existing Vulkan path; do not reintroduce
  `SetOverlayRaw` as a convenient live diagnostic shortcut.
- A future explicitly authorized experiment should separate raw upload from
  immediate teardown, vary only one factor at a time, and capture process/journal
  evidence from a session outside the headset graphical service. A longer-lived
  probe is an experiment, not a proven workaround; adding a sleep is not a fix.
- Tmux protects a remote coding process from a terminal/SSH disconnect. It does
  not itself isolate the headset compositor, and a tmux server inside a killed
  service group could still die. Confirm where the server lives before replaying.
