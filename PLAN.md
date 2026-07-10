# PLAN.md — Wii U Remote Co-op ("Parsec for Wii U")

## 1. Goal

Let up to **4 players**, each on their own **Windows PC** with **any Nintendo controller** plugged into that PC, play a single **Wii U** remotely — as if they were all sitting on the couch. The PC captures each player's controller and routes it to the Wii U; the Wii U streams its video/audio back out to everyone.

This is the **local-multiplayer-over-the-internet** model (exactly what Parsec's "friends play" does): one shared Wii U screen streamed to everyone, four separate controllers injected as Player 1–4.

**Realistic expectations up front:** low resolution (roughly 428×240 to ~480p), noticeable latency, and some performance cost to the running Wii U game because video is software-encoded. Great for party/turn-based/local-co-op games (Smash, Mario Kart 8, NSMBU, Nintendo Land). Not suitable for competitive twitch play.

---

## 2. Reality Check — what already exists

You are **not** starting from zero. Two existing homebrew projects already solve the two hardest pieces. The project is largely **integration + modernization + a unified PC app**, not green-field research.

| Piece | Prior art | State |
|---|---|---|
| Route PC controllers → Wii U as up to 4 controllers over network | **HID to VPAD** (`Maschell/hid_to_vpad`, WUPS branch) | Working; emulates GamePad + up to 4 Pro Controllers; has a network client protocol (v3) with rumble |
| Capture Wii U TV/GamePad screen → stream to PC | **StreamingPluginWiiU** (`Maschell/StreamingPluginWiiU`) | Early PoC; ~428×240; **software** encoding; can hurt game performance |
| Decode H.264 on Wii U (reference for the *reverse* direction / decoders) | **moonlight-wiiu** (`GaryOderNichts/moonlight-wiiu`) | Mature; uses HW h264 **decode** |

**Key technical limit:** the Wii U's hardware H.264 **encoder** (the DRC/GamePad path) is proprietary and not exposed to homebrew in a usable way. Video **out** must be software-encoded (MJPEG or a lightweight H.264/custom codec) at low res. Hardware **decode** is available, but that only helps traffic coming *into* the Wii U, not going out.

---

## 3. Architecture

**Recommended topology: host-relay (star through one PC), not every-PC-direct-to-console.**

```
                       LAN (same house as the Wii U)
  ┌─────────────┐     video out ──────────►  ┌──────────────────┐
  │   Wii U     │                             │   HOST PC        │
  │  (Aroma +   │  ◄──── 4x controller in ──  │  (your strong PC)│
  │  2 plugins) │                             │  = the "server"  │
  └─────────────┘                             └───────┬──────────┘
                                                       │  internet
                            ┌──────────────────────────┼───────────────────┐
                            ▼                           ▼                   ▼
                     ┌────────────┐            ┌────────────┐        ┌────────────┐
                     │ Player 2 PC│            │ Player 3 PC│        │ Player 4 PC│
                     │ + controller│           │ + controller│       │ + controller│
                     └────────────┘            └────────────┘        └────────────┘
              (Player 1 = the host PC itself, also with a controller)
```

**Why host-relay instead of 4 direct connections to the Wii U:**
- The Wii U is weak and behind home NAT. Making it serve 4 video copies + accept 4 remote peers directly is brutal on its CPU/Wi-Fi and a NAT-traversal nightmare.
- With host-relay, the **Wii U only ever talks to ONE local machine** (the host PC on the same LAN). It sends **one** video stream and receives **one** input stream (carrying up to 4 controllers).
- All the hard multi-client / internet / NAT / fan-out work lives on the **host PC**, which is powerful and easy to develop for. The host also **transcodes** the low-res software stream into efficient H.264 for remote players and **fans it out**.

This keeps the painful-to-develop console side as dumb and light as possible.

**Data flows:**
- **Video/Audio:** Wii U → (LAN) → Host PC → transcode/H.264 → (internet) → Players 2–4 + local display for Player 1.
- **Input:** Players 2–4 controllers → (internet) → Host PC → merged with Player 1's local controller → (LAN) → Wii U → injected as VPAD/KPAD.

---

## 4. Components to Build

### 4A. Wii U side — Aroma WUPS plugin(s)

Runs under the **Aroma** environment (current standard). Plugins auto-load and, unlike the old Homebrew Launcher days, **multiple plugins can run alongside a retail game at once** — which is exactly what we need (video-out + input-in simultaneously).

1. **Video Capture Module**
   - Grab the TV framebuffer (and optionally the GamePad/DRC framebuffer) each frame.
   - Study StreamingPluginWiiU's capture hooks as the starting point.
   - Downscale to target res (configurable: 428×240 default, up to ~480p).
   - Color-convert as needed (framebuffer format → encoder input format).

2. **Software Encoder**
   - MVP: **MJPEG** (simplest, per-frame, resilient to packet loss, cheap-ish).
   - Stretch: lightweight H.264 software encoder, or a custom delta/RLE codec for lower bandwidth.
   - Must be tunable (resolution, framerate cap, quality) to trade off game-performance vs. picture. Expose via the WUPS config menu (L + D-Pad-Down + Minus style overlay).

3. **Audio Capture + Encode**
   - Tap the Wii U audio mixer output.
   - Encode (Opus if feasible on-CPU, else raw PCM / ADPCM at low sample rate) — keep it cheap; audio can't steal too much CPU from the game.

4. **Network Sender (to host PC only)**
   - UDP for video/audio (loss-tolerant, low latency), with a small sequencing/timestamp header.
   - Handles a single peer: the host PC on the LAN.

5. **Input Receiver + Injector**
   - Receive up to 4 controllers' state from the host PC.
   - Inject by patching `VPADRead` (GamePad) and `KPADRead`/`WPADRead` (Pro Controllers / Wiimotes) — the same mechanism HID to VPAD / `controller_patcher` uses. **Reuse that engine.**
   - Player 1 → GamePad (VPAD) or Pro Controller; Players 2–4 → Pro Controllers. Note: using a 4th Pro Controller may require the GamePad to be "disabled"/not counted — replicate HID to VPAD's handling.
   - Pass rumble state back (optional, per-player).

6. **Session / Pairing / Config**
   - Simple handshake with the host PC (protocol version, negotiate res/fps, assign controller slots).
   - Show Wii U IP on screen for easy connect (like ftpiiu does).
   - Config menu for resolution/fps/quality/which-screen.

> The two responsibilities (video-out, input-in) can be **one combined plugin** or **two plugins**. Recommendation: **two plugins** initially (fork StreamingPluginWiiU for video, fork HID to VPAD for input), then optionally merge once both are stable. Two plugins let you lean directly on the existing codebases.

### 4B. PC side — Windows app (the big new build; this is where most of *your* code goes)

This single app has **two roles** selectable at launch: **Host** (the machine on the LAN with the Wii U) or **Client** (a remote friend). Most code is shared.

**Shared / core:**
1. **Controller Input Layer — "any Nintendo controller"**
   - Base: **SDL3 GameController API** (covers Switch Pro, most 8BitDo, generic pads out of the box).
   - Supplement with **hidapi** for Nintendo-specific devices SDL doesn't fully cover:
     - **GameCube controllers** via the official WUP-028 USB adapter (reuse Dolphin's adapter driver approach).
     - **Joy-Cons** (single/pair, incl. motion) — reuse BetterJoy/joycon-driver logic.
     - **Wii U Pro Controller / Wii Remote** over Bluetooth if you want full coverage.
   - Normalize everything into a **canonical controller state struct** (buttons, 2 sticks, triggers, optional gyro/accel).
   - Map canonical state → Wii U target profile (VPAD vs. Pro Controller button layouts). Configurable remapping.
   - **Known gaps to document:** GamePad **touchscreen** and **motion** have no source on most pads (Joy-Con/Pro have gyro you *could* map; touch could be mapped to mouse or a click-region). Flag these as limitations for touch-dependent games.

2. **Video/Audio Decoder + Display**
   - Decode incoming MJPEG/H.264 (use FFmpeg/libav).
   - Low-latency render (SDL, or a lightweight D3D/GL window).
   - A/V sync + jitter buffer (keep it small; prioritize latency over smoothness).

3. **Network protocol implementation** (see §5).

**Host role (extra):**
4. **Wii U link (LAN):** receive the single video/audio stream from the Wii U; send the merged 4-controller input stream to the Wii U.
5. **Transcoder:** re-encode the Wii U's low-res software stream into efficient **H.264** (hardware NVENC on your RTX card) for sending to remote players. Your PC is strong enough to do this comfortably.
6. **Fan-out / session server:** accept up to 3 remote clients, assign them controller slots (P2–P4), broadcast the transcoded video, collect their inputs.
7. **NAT traversal / connectivity:** for internet play — STUN/UDP hole-punching, or ship a simple relay fallback. (MVP can be LAN-only or require port-forwarding.)
8. **Lobby UI:** show connected players, slot assignments, per-player latency, stream settings.

**Client role (extra):**
9. Connect to a host (IP/code), claim a player slot, stream video in, stream local controller out. Minimal UI.

---

## 5. Network Protocol

- **Transport:** UDP for media + input (latency-critical, loss-tolerant); a small reliable/TCP or reliable-UDP side-channel for handshake, config, and slot assignment.
- **Ports:** pick fixed defaults + make configurable; document firewall/port-forward needs.
- **Packet types (minimum):**
  - `HELLO` / `HELLO_ACK` — version, role, capabilities.
  - `CONFIG` — resolution, fps, codec, bitrate, slot assignment.
  - `VIDEO` — seq no., timestamp, frame/slice payload, keyframe flag.
  - `AUDIO` — seq no., timestamp, payload.
  - `INPUT` — player slot, button bitfield, stick axes, triggers, (optional) gyro; sent at fixed high rate (e.g. 120–250 Hz) with latest-state-wins.
  - `RUMBLE` (optional) — per-slot.
  - `PING`/`PONG` — latency measurement + keepalive.
- **Design rules:** input packets carry full current state (not deltas) so a dropped packet self-heals next tick. Video tolerates loss; request keyframe on corruption.

---

## 6. Build Phases / Milestones

**Phase 0 — Prove the pieces separately (mostly using existing tools).**
- Get **HID to VPAD** (network client) working: control a Wii U game from a PC controller over the network.
- Get **StreamingPluginWiiU** building under **Aroma** and streaming TV → PC (this is the riskiest existing pilot; it's orphaned, so budget time to modernize the build).
- Outcome: you've personally confirmed both directions work on your hardware, and you understand both codebases.

**Phase 1 — Single player, end to end, LAN.**
- One PC (host role only), one controller, one Wii U.
- Your unified PC app: receive video + display, read one controller + inject.
- Get latency acceptable on LAN at 428×240.

**Phase 2 — Four local input slots.**
- Inject P1–P4 on the Wii U from four controllers plugged into the **host** PC. (No remote players yet.) Test in Mario Kart 8 / Smash / Nintendo Land.

**Phase 3 — Remote players (internet).**
- Add host fan-out + NVENC transcode + client app.
- Players 2–4 connect over the internet, claim slots, stream video, send input.
- Start LAN-only or port-forward; add NAT traversal after.

**Phase 4 — Polish.**
- Lobby UI, per-player latency display, adaptive bitrate, controller remapping UI, reconnect handling, audio sync tuning, config presets.

---

## 7. Recommended Tech Stack

- **Wii U plugins:** C/C++ with **WUT** (Wii U Toolchain) + **WUPS** (Wii U Plugin System) under **Aroma**. Reuse `controller_patcher` (input) and StreamingPluginWiiU's capture (video).
- **PC app:** C++ (or Rust). Libraries:
  - **SDL3** — controllers + window/render.
  - **hidapi** — GameCube adapter, Joy-Cons, Wii U Pro.
  - **FFmpeg/libav** — decode + (host) NVENC transcode.
  - Networking: raw UDP + a small reliability layer, or a library like **ENet** for reliable-UDP channels.
- **Repos to study/fork:** `Maschell/hid_to_vpad`, `Maschell/StreamingPluginWiiU`, `GaryOderNichts/moonlight-wiiu`, `wiiu-env/*` (plugin backend, toolchain).

---

## 8. Key Risks & Open Questions

1. **Software-encode performance hit (biggest risk).** Stealing Wii U CPU to encode video will slow/stutter the running game. Mitigation: keep res/fps low, cheap codec (MJPEG), heavy tuning. This is a hard ceiling — manage expectations.
2. **Latency stack.** Capture + software encode + LAN + host transcode + internet + decode = meaningful end-to-end delay. Fine for party games, bad for competitive.
3. **Modernizing orphaned code.** StreamingPluginWiiU is an old PoC; getting it building/running cleanly under current Aroma is real work.
4. **Plugin coexistence.** Confirm the video plugin + input plugin run together stably alongside a retail game under Aroma (Aroma supports multiple plugins, but test for conflicts/priority/CPU contention).
5. **4th controller / GamePad handling.** Replicate HID to VPAD's constraint handling (4th Pro Controller may need GamePad disabled).
6. **Touchscreen & motion inputs.** No good source from most PC controllers; touch-heavy Wii U games will be limited.
7. **NAT traversal for internet play.** Needs STUN/hole-punching or a relay; MVP can require port-forwarding.
8. **Audio CPU budget.** Keep audio encode cheap so it doesn't compound the video performance hit.

---

## 9. Summary

- ~50% is prior art: **HID to VPAD** (input routing, 4 controllers, network) and **StreamingPluginWiiU** (video out). 
- The **new** work is a **unified Windows host/client app** (controller normalization for any Nintendo pad, video decode/display, host-side transcode + fan-out + lobby + NAT) plus **modernizing and wiring together the two Wii U plugins** under Aroma.
- **Host-relay topology** keeps the Wii U talking to just one LAN PC; that PC does all the heavy multi-player/internet lifting.
- Set expectations: **low-res, some latency, party/co-op focused** — not a crisp competitive experience.
