# Persistent Vulkan overlay deployment — 2026-09-24

Source commit: `4c043c99`.
Installed native ARM64 version: `2026-09-24T181314Z-g4c043c99`.

The user requested replacing raw uploads with a GPU texture and supplied the
current Frame SSH target and key for native build/install verification. These
are dated observations, not future device availability or permission to launch
hardware checks.

## Implementation

The overlay now uses `SetOverlayTexture` with a persistent Vulkan RGBA8 image.
The CPU panel rasterizer still produces the pixels. One staging allocation,
image and command buffer are reused; redraws do not recreate them. SteamVR
selects the physical device and required instance/device extensions. Transfers
share one FrameYap-owned graphics queue with SteamVR, and GPU resources survive until
`VR_Shutdown` completes. No desktop surface, swapchain or raw-upload fallback
was introduced. See [rendering details](../overlay.md).

## Verification

- Default local offline build: 13/13 CTest checks passed.
- Local native build: all 16 checks passed; the fake Wayland protocol check
  required permission to bind its local test socket outside the sandbox.
- Native ARM64 Release build on Frame from a checksum-verified Git archive:
  16/16 hardware-free checks passed, including the Vulkan fake-driver test.
- Explicit offscreen Vulkan check on the workstation's RTX 4090, with
  `VK_LAYER_KHRONOS_validation` enabled: eight exact 1000×680 RGBA readbacks,
  one stable image, no validation messages.
- The same explicit offscreen check on Frame's `Turnip Adreno (TM) 750`:
  eight exact 1000×680 RGBA readbacks and one stable image. This exercised the
  actual image upload/layout/readback path, without OpenVR initialization.
- Staged and installed native dependency resolution succeeded. Vulkan resolves
  to the system loader; SDL/OpenVR resolve inside FrameYap's own `lib/`.
- The native archive was checksum-verified and installed. `current` selects
  the version above; `previous` retains `2026-09-24T174700Z-gc59c8b1`.
  Installed `--version` matched and its executable hash matched the staged
  binary. No app, SSH or user-session process was terminated.

Native archive SHA-256:
`d6f0173b25a879c02f0ee67063c881a23dff874a71674826ddd7666d0980759d`.
Installed executable SHA-256:
`761ea0e4d2dc235c5f056d8d944cb68392713c246aaec008d0b6c235954ef645`.

## Acceptance boundary

The updated headset installation is verified, but this session did not launch
an OpenVR visual/controls probe or collect a wearer report. GPU readback does
not establish SteamVR texture acceptance, orientation, click behavior or a
flicker fix. No microphone, inference or text/Enter delivery was exercised.
The next human headset check should compare both static action clicks and
content-changing tabs using the installed Vulkan build's `--check-controls`.
