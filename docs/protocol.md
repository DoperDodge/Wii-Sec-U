# Wii-Sec-U wire protocol (v1)

Single source of truth: [`protocol/wsu_protocol.h`](../protocol/wsu_protocol.h)
(plain C99, shared verbatim between the big-endian Wii U plugins and the
little-endian PC app; all multi-byte fields are **big-endian** and packed
byte-wise, so no struct-cast tricks and no padding surprises).

Transport is **UDP only**, per PLAN.md §5: media and input are
latency-critical and loss-tolerant. There is deliberately no reliability
layer in v1 — the handshake retries by resending HELLO once per second,
input packets carry full state so any drop self-heals on the next tick,
and an incompletely received video frame is simply dropped.

## Ports

| Port | Owner | Purpose |
|---|---|---|
| 4404/udp | `wsu-input` plugin (console) | HELLO/PING in, INPUT_BUNDLE in |
| 4405/udp | `wsu host` (host PC) | remote clients (HELLO/INPUT in, VIDEO/AUDIO/CONFIG out) |
| 4406/udp | `wsu-stream` plugin (console) | HELLO/PING in, VIDEO/AUDIO/CONFIG out |

Remote players only ever need to reach **4405** on the host (port-forward
that one). The console ports never leave the LAN.

## Common header (8 bytes)

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `"WSU1"` |
| 4 | 1 | protocol version (1) |
| 5 | 1 | packet type |
| 6 | 2 | sequence number (per sender; wraps) |

## Packet types

| type | name | payload |
|---|---|---|
| 1 | `HELLO` | role u8 (1 host, 2 client), flags u8, name char[16] |
| 2 | `HELLO_ACK` | status u8 (0 ok, 1 full, 2 version mismatch, 3 rejected), slot u8 (0–3 or 0xFF) |
| 3 | `CONFIG` | width u16, height u16, fps u8, videoCodec u8 (0 MJPEG, 1 RAWRGB-test), quality u8, audioCodec u8 (0 none, 1 PCM16), audioRateHz u16 |
| 4 | `VIDEO` | frameId u32, timestampMs u32, flags u8 (bit0 keyframe), sliceIndex u16, sliceCount u16, payloadLen u16, payload ≤1200 B |
| 5 | `AUDIO` | timestampMs u32, payloadLen u16, PCM16 BE interleaved stereo |
| 6 | `INPUT` | timestampMs u32 + one input body (client → host) |
| 7 | `INPUT_BUNDLE` | timestampMs u32, count u8, count × input body (host → console) |
| 8 | `RUMBLE` | slot u8, intensity u8 *(reserved, not yet produced)* |
| 9 | `PING` | timestampMs u32 |
| 10 | `PONG` | echoed timestampMs u32 |
| 11 | `BYE` | reason u8 (0 quit, 1 timeout, 2 kicked, 3 shutdown) |
| 12 | `KEYFRAME_REQ` | — *(reserved for future delta codecs; MJPEG frames are all keyframes)* |

## Input body (26 bytes, full state every packet)

| field | size | notes |
|---|---|---|
| slot | u8 | 0–3; server-authoritative on the host |
| flags | u8 | bit0 touching, bit1 gyro valid |
| buttons | u32 | canonical bitfield (`WsuButtons`): A/B/X/Y, D-pad, L/R/ZL/ZR, +/−/HOME, stick clicks, TV |
| lx, ly, rx, ry | 4 × i16 | −32767…32767, +x right, +y up |
| lt, rt | 2 × u8 | analog triggers (GameCube pads) |
| touchX, touchY | 2 × u16 | 0…4095 normalized; 0xFFFF = none |
| gyroPitch/Yaw/Roll | 3 × i16 | deg/s × 16 |

The console plugin maps canonical buttons onto `VPAD_BUTTON_*` for slot 0
(GamePad) and `WPAD_PRO_BUTTON_*` for slots 1–3 (Pro Controllers),
including the stick-emulation bits some games read.

## Session flows

**Host ⇄ console** — the host sends `HELLO(role=host)` to both console
ports once per second until each plugin replies `HELLO_ACK`. The stream
plugin starts sending `CONFIG` (repeated every 2 s) plus `VIDEO`/`AUDIO`
to the HELLO's source address. The host sends `INPUT_BUNDLE` at ~120 Hz
and `PING` at 1 Hz to both plugins; 5 s of silence on either side tears
the link down (console pauses streaming and neutralizes inputs after 1 s;
host falls back to HELLO retries).

**Client ⇄ host** — same pattern on port 4405: `HELLO(role=client, name)`
→ `HELLO_ACK(slot)` + `CONFIG`, then `VIDEO`/`AUDIO` fan-out toward the
client, `INPUT` at ~120 Hz + `PING` at 1 Hz back. The host relays the
console's VIDEO/AUDIO datagrams **byte-for-byte** (same frameIds and
slice boundaries everywhere; no transcode in v1).

**Timeout invariants** — input slots go neutral after
`WSU_INPUT_TIMEOUT_MS` (1000 ms) without an update; peers are declared
gone after `WSU_PEER_TIMEOUT_MS` (5000 ms) without any packet.

## Video slicing

Frames (one JPEG each for MJPEG) are split into ≤1200-byte slices, each
carrying `(frameId, sliceIndex, sliceCount)`. The receiver reassembles
with a 4-frame pending window, latest-wins: completing a frame abandons
every older incomplete frame, and slices at or before the newest
delivered frame are discarded (`pc/src/core/frame_assembler.cpp`).
