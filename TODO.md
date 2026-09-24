# FrameYap TODO

- [ ] Resolve the Redux inference runtime license before further inference or a
  bundled release. Obtain written permission covering use and redistribution of
  M87 Labs Kestrel kernels (including the CPU payload), or scope an independently
  licensed runtime for the same weights. Inventory the other runtime dependencies'
  notices/licenses before publishing. The CC-BY-4.0 model weights do not grant
  permission to use or redistribute the proprietary runtime. See
  [third-party notes](docs/third-party.md).
- [x] Validate a basic menu launch on Frame: OpenVR registration did **not** show
  FrameYap in the first checked dashboard menu; the user found FrameYap in Steam's
  **Non-Steam** section and reported that selecting it showed a panel and Quit
  worked. The server log showed a `--run` overlay connection followed by exit;
  no FrameYap process remained. The installed native-only package had no bundled
  runtime or model. This does not validate transcription, recording, text delivery,
  cold startup, or which shortcut discovery mechanism Steam used.
- [ ] Verify the exact missing-runtime panel message and Steam's shortcut
  persistence across a normal restart (without restarting sessions just for the
  test). Confirm no runtime/model environment override before future live checks.
