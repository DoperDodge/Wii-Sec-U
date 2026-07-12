# Known limitations & open risks

The hard ceilings come from the console itself (PLAN.md §2/§8); the rest
is v0.1 scope that has a clear path forward.

## Inherent (manage expectations)

- **Low resolution, software encode.** The Wii U's hardware H.264 encoder
  is not usable from homebrew, so the TV picture is downscaled (428×240
  default, up to ~480p) and JPEG-encoded on a CPU core. Encoding steals
  CPU from the game — lower the frame rate/quality in the plugin's config
  menu (L + D-Pad Down + SELECT in-game) if a game stutters. This is a
  hard ceiling, not a bug.
- **Latency.** Capture (+1 frame by design) + encode + LAN + relay +
  internet + decode adds up. Party/co-op/turn-based games: great.
  Competitive twitch play: no.
- **Bandwidth.** MJPEG at 428×240@20 ≈ 3–6 Mbit/s per remote player
  (the host uploads that to each client). Host-side H.264 transcode
  (NVENC) is the planned fix and slots in at the relay point.

## Current implementation gaps (v0.1)

- **Not yet validated on hardware.** The plugins compile against current
  wut/WUPS and copy proven hooking patterns, but the GX2 copy/read timing,
  KPAD injection compatibility matrix, and audio tap all need real-console
  shakedown. Treat the console side as beta groundwork.
- **GPU/CPU sync is heuristic.** The capture surface is read one scan-out
  (~16 ms) after the blit is issued, without a GPU fence. If tearing or
  stale frames show up on hardware, the fix is a `GX2DrawDone`-free fence
  (e.g. `GX2SetGPUFinishedCallback`-style) in `capture.cpp`.
- **Connect-callback emulation is missing.** Games that discover
  controllers only via `WPADSetConnectCallback` (instead of polling
  `WPADProbe`) won't see P2–P4 yet. hid_to_vpad's controller_patcher
  solves this; same approach applies.
- **GamePad touch/motion for remote players.** Touch is injected (mapped
  through raw panel coordinates) but most PC pads have no touch source;
  gyro fields exist on the wire but are not yet injected. Touch-dependent
  games are limited by physics, per PLAN.md §8.6.
- **No rumble return path** (`RUMBLE` is reserved in the protocol).
- **No NAT traversal.** Remote players need the host to port-forward UDP
  4405 (or a VPN like Tailscale). STUN/hole-punching is Phase 3 polish.
- **No encryption/auth.** The session is open UDP — anyone who can reach
  port 4405 can claim a slot. Fine on LAN/VPN; add a session token before
  exposing it to the raw internet for strangers.
- **Decode is CPU-side stb_image, not FFmpeg.** The display window
  (`WSU_WITH_SDL`) decodes MJPEG with the vendored stb_image — ample for
  428×240 streams, but a future FFmpeg path would cut client CPU and is
  the prerequisite for host-side NVENC transcode (lower remote
  bandwidth). Headless builds still relay without decoding.
- **4th Pro Controller quirk.** Using P4 may require the GamePad to be
  "disabled" in some titles (hid_to_vpad has the same constraint,
  PLAN.md §8.5). Untested until hardware validation.
