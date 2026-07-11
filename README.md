# Wii-Sec-U — "Parsec for Wii U"

Play a single Wii U with up to **4 players over the internet**: each player
plugs a controller into their own Windows/Linux PC, the Wii U's picture and
sound stream out to everyone, and all four controllers are injected back
into the console as players 1–4 — remote couch co-op.

The full design rationale lives in [PLAN.md](PLAN.md). This repository
implements it.

```
                       LAN (same house as the Wii U)
  ┌─────────────┐     video/audio ─────────►  ┌──────────────────┐
  │   Wii U     │                             │   HOST PC        │
  │  (Aroma +   │  ◄──── 4x controller in ──  │   `wsu host`     │
  │  2 plugins) │                             └───────┬──────────┘
  └─────────────┘                                     │ internet
                          ┌───────────────────────────┼───────────────────┐
                          ▼                           ▼                   ▼
                   ┌────────────┐              ┌────────────┐      ┌────────────┐
                   │ Player 2 PC│              │ Player 3 PC│      │ Player 4 PC│
                   │`wsu client`│              │`wsu client`│      │`wsu client`│
                   └────────────┘              └────────────┘      └────────────┘
            (Player 1 = the host PC itself, also with a controller)
```

## What's in this repo

| Component | Path | What it does |
|---|---|---|
| **wsu-input** plugin | [`wiiu/input-plugin/`](wiiu/input-plugin/) | Aroma (WUPS) plugin: receives up to 4 controllers over UDP and injects them via `VPADRead`/`KPADRead(Ex)`/`WPADProbe` hooks (P1 → GamePad, P2–P4 → Pro Controllers) |
| **wsu-stream** plugin | [`wiiu/stream-plugin/`](wiiu/stream-plugin/) | Aroma (WUPS) plugin: captures the TV framebuffer on scan-out (GX2 hook), software-encodes MJPEG on a background core, taps TV audio via the AX final-mix callback, streams to the host PC |
| **wsu** PC app | [`pc/`](pc/) | One binary, three roles: `host` (LAN link to the console + fan-out server + lobby), `client` (remote player), `console-sim` (fake Wii U for development without hardware) |
| Shared protocol | [`protocol/wsu_protocol.h`](protocol/wsu_protocol.h) | Endian-safe single-header UDP wire protocol used by all of the above ([spec](docs/protocol.md)) |

## Status

Phases from [PLAN.md §6](PLAN.md):

- [x] **Phase 1 groundwork** — full PC pipeline works end-to-end *against the
  console simulator*: video capture → slicing → host relay → client
  reassembly, plus 4-slot input merge back. Covered by an automated
  integration test (`pc/tests/test_end_to_end.cpp`).
- [x] **Phase 2 groundwork** — 4 input slots merged host-side, injected via
  the wsu-input plugin's VPAD/KPAD hooks.
- [x] **Phase 3 groundwork** — remote clients join the host over UDP, claim
  slots P2–P4, receive the relayed stream (port-forward or LAN; no NAT
  traversal yet).
- [x] **Video display** — build with `-DWSU_WITH_SDL=ON` and the host and
  every remote client get a window showing the Wii U's picture (MJPEG
  decoded via vendored stb_image, letterboxed SDL renderer) plus PCM
  audio playback. Clients default to it; hosts opt in with
  `--display sdl`.
- [ ] **On-hardware validation** — the plugins compile against current
  wut/WUPS and follow the proven hooking patterns (hid_to_vpad,
  StreamingPluginWiiU, SwipSwapMe), but have not yet been exercised on a
  real console. Expect iteration here.
- [ ] FFmpeg/NVENC transcode for lower remote bandwidth, NAT traversal,
  lobby UI, rumble, connect-callback emulation.

Read [docs/limitations.md](docs/limitations.md) before setting expectations —
low resolution, real latency, party-game territory (by design; see
PLAN.md §1).

## Quick start

### PC app (no console needed)

```sh
cmake -S pc -B pc/build -DCMAKE_BUILD_TYPE=Release
cmake --build pc/build --parallel
ctest --test-dir pc/build          # all green without any hardware

# terminal 1: fake Wii U
./pc/build/wsu console-sim --log-input
# terminal 2: host (finds the sim via loopback broadcast)
./pc/build/wsu host --console 127.0.0.1 --input scripted
# terminal 3: remote player
./pc/build/wsu client --host 127.0.0.1 --name luigi --input scripted
```

You'll see the host report `video ...fps`, the client report the same
stream, and the sim print the merged P1/P2 controller activity.

### Real console

1. Install [Aroma](https://aroma.foryour.cafe/), copy
   `wsu-input.wps` and `wsu-stream.wps` to `sd:/wiiu/environments/aroma/plugins/`
   (build them per [docs/building.md](docs/building.md) or grab the CI
   artifacts).
2. On the host PC: `wsu host --console <wiiu-ip>` (or omit `--console` to
   discover via LAN broadcast). Add `--display sdl` if the host wants a
   window too.
3. Remote players: `wsu client --host <host-ip-or-ddns> --name mario` —
   with an SDL build this opens a window showing the Wii U's screen with
   audio (the host forwards UDP 4405).
4. Real gamepads and the display window both come from the SDL build:
   `-DWSU_WITH_SDL=ON`, then `--input sdl`. `--input scripted` and
   `--display stats` exist for headless wiring tests.

## Building

See [docs/building.md](docs/building.md) — PC needs CMake + a C++17
compiler (zero mandatory dependencies); the plugins build via the
`wiiu/Dockerfile` devkitPPC image, same as every wiiu-env plugin.

## License

[GPL-3.0-or-later](LICENSE). The Wii U side deliberately follows the
architecture of Maschell's GPL homebrew (hid_to_vpad / controller_patcher,
StreamingPluginWiiU) so future code-sharing stays license-compatible.
