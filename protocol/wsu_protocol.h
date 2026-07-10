/*
 * wsu_protocol.h — Wii-Sec-U shared wire protocol (see docs/protocol.md)
 *
 * Single header shared between the Wii U plugins (big-endian PowerPC,
 * compiled with devkitPPC) and the PC host/client app (little-endian,
 * MSVC/GCC/Clang). Plain C99, no allocation, no dependencies beyond
 * <stdint.h>/<string.h>. All wire values are big-endian and are read and
 * written byte-by-byte, so the same code is correct on both architectures.
 *
 * Transport is UDP. Every datagram starts with an 8-byte header:
 *
 *   offset  size  field
 *   0       4     magic "WSU1"
 *   4       1     protocol version
 *   5       1     packet type (WsuPacketType)
 *   6       2     sequence number (per-sender, per-stream; wraps)
 *
 * Design rules (PLAN.md §5): input packets carry full current state so a
 * dropped packet self-heals on the next tick; video is sliced below typical
 * MTU and tolerates loss — an incomplete frame is simply dropped.
 *
 * This file is part of Wii-Sec-U.
 * Licensed under the GNU General Public License v3.0 or later.
 */
#ifndef WSU_PROTOCOL_H
#define WSU_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define WSU_PROTO_MAGIC 0x57535531u /* "WSU1" */
#define WSU_PROTO_VERSION 1u

/* UDP ports. The console runs two independent plugins, each owning one
 * socket; the host PC owns one public socket for remote clients. */
#define WSU_CONSOLE_INPUT_PORT 4404  /* wsu-input plugin listens here   */
#define WSU_HOST_PORT 4405           /* host PC listens for clients     */
#define WSU_CONSOLE_STREAM_PORT 4406 /* wsu-stream plugin listens here  */

#define WSU_HEADER_SIZE 8
#define WSU_MAX_PACKET 1400        /* largest datagram we ever send     */
#define WSU_MAX_SLICE_PAYLOAD 1200 /* video/audio payload bytes per pkt */

#define WSU_MAX_PLAYERS 4
#define WSU_NO_SLOT 0xFF

/* Analog ranges on the wire. */
#define WSU_STICK_MAX 32767  /* sticks: -32767..32767 (i16)            */
#define WSU_TOUCH_MAX 4095   /* touch: 0..4095 normalized               */
#define WSU_TOUCH_NONE 0xFFFF

/* Suggested cadences (informational, not enforced). */
#define WSU_INPUT_RATE_HZ 120
#define WSU_INPUT_TIMEOUT_MS 1000 /* slot inactive after this silence   */
#define WSU_PEER_TIMEOUT_MS 5000  /* peer considered gone after this    */

/* ------------------------------------------------------------------ */
/* Enums                                                              */
/* ------------------------------------------------------------------ */

typedef enum WsuPacketType {
    WSU_PKT_HELLO = 1,
    WSU_PKT_HELLO_ACK = 2,
    WSU_PKT_CONFIG = 3,
    WSU_PKT_VIDEO = 4,
    WSU_PKT_AUDIO = 5,
    WSU_PKT_INPUT = 6,
    WSU_PKT_INPUT_BUNDLE = 7,
    WSU_PKT_RUMBLE = 8,
    WSU_PKT_PING = 9,
    WSU_PKT_PONG = 10,
    WSU_PKT_BYE = 11,
    WSU_PKT_KEYFRAME_REQ = 12,
} WsuPacketType;

typedef enum WsuRole {
    WSU_ROLE_HOST = 1,   /* host PC talking to the console or serving  */
    WSU_ROLE_CLIENT = 2, /* remote player PC talking to the host       */
} WsuRole;

typedef enum WsuHelloStatus {
    WSU_HELLO_OK = 0,
    WSU_HELLO_FULL = 1,
    WSU_HELLO_VERSION_MISMATCH = 2,
    WSU_HELLO_REJECTED = 3,
} WsuHelloStatus;

typedef enum WsuVideoCodec {
    WSU_CODEC_MJPEG = 0,  /* one JPEG image per frame                   */
    WSU_CODEC_RAWRGB = 1, /* uncompressed RGB24, used by the console
                             simulator and loopback tests               */
} WsuVideoCodec;

typedef enum WsuAudioCodec {
    WSU_AUDIO_NONE = 0,
    WSU_AUDIO_PCM16 = 1, /* interleaved stereo int16, big-endian        */
} WsuAudioCodec;

typedef enum WsuByeReason {
    WSU_BYE_QUIT = 0,
    WSU_BYE_TIMEOUT = 1,
    WSU_BYE_KICKED = 2,
    WSU_BYE_SHUTDOWN = 3,
} WsuByeReason;

/* Canonical controller buttons. The PC app normalizes every physical pad
 * into this bitfield; the console plugin maps it onto VPAD (GamePad) or
 * WPAD Pro Controller bits depending on the slot. */
enum WsuButtons {
    WSU_BTN_A = 1u << 0,
    WSU_BTN_B = 1u << 1,
    WSU_BTN_X = 1u << 2,
    WSU_BTN_Y = 1u << 3,
    WSU_BTN_UP = 1u << 4,
    WSU_BTN_DOWN = 1u << 5,
    WSU_BTN_LEFT = 1u << 6,
    WSU_BTN_RIGHT = 1u << 7,
    WSU_BTN_L = 1u << 8,
    WSU_BTN_R = 1u << 9,
    WSU_BTN_ZL = 1u << 10,
    WSU_BTN_ZR = 1u << 11,
    WSU_BTN_PLUS = 1u << 12,
    WSU_BTN_MINUS = 1u << 13,
    WSU_BTN_HOME = 1u << 14,
    WSU_BTN_STICK_L = 1u << 15,
    WSU_BTN_STICK_R = 1u << 16,
    WSU_BTN_TV = 1u << 17, /* GamePad TV button, no Pro equivalent      */
};

/* WsuInputState.flags bits. */
#define WSU_INPUT_FLAG_TOUCHING 0x01 /* touchX/touchY are a live touch  */
#define WSU_INPUT_FLAG_GYRO 0x02     /* gyro fields are meaningful      */

/* Video packet flags. */
#define WSU_VIDEO_FLAG_KEYFRAME 0x01

/* ------------------------------------------------------------------ */
/* Byte-order helpers (wire = big-endian)                             */
/* ------------------------------------------------------------------ */

static inline void wsu_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void wsu_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline void wsu_put_i16(uint8_t *p, int16_t v) {
    wsu_put_u16(p, (uint16_t)v);
}

static inline uint16_t wsu_get_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t wsu_get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int16_t wsu_get_i16(const uint8_t *p) {
    return (int16_t)wsu_get_u16(p);
}

/* ------------------------------------------------------------------ */
/* Header                                                             */
/* ------------------------------------------------------------------ */

typedef struct WsuHeader {
    uint8_t version;
    uint8_t type;
    uint16_t seq;
} WsuHeader;

/* Writes the common header; returns WSU_HEADER_SIZE. `buf` must hold at
 * least WSU_HEADER_SIZE bytes. */
static inline int wsu_pack_header(uint8_t *buf, uint8_t type, uint16_t seq) {
    wsu_put_u32(buf, WSU_PROTO_MAGIC);
    buf[4] = WSU_PROTO_VERSION;
    buf[5] = type;
    wsu_put_u16(buf + 6, seq);
    return WSU_HEADER_SIZE;
}

/* Validates magic and parses the header. Returns WSU_HEADER_SIZE on
 * success, -1 if the datagram is not a WSU packet. A version mismatch is
 * NOT an error here — callers decide how to respond (HELLO_ACK carries a
 * version-mismatch status). */
static inline int wsu_parse_header(const uint8_t *buf, size_t len,
                                   WsuHeader *out) {
    if (len < WSU_HEADER_SIZE || wsu_get_u32(buf) != WSU_PROTO_MAGIC) {
        return -1;
    }
    out->version = buf[4];
    out->type = buf[5];
    out->seq = wsu_get_u16(buf + 6);
    return WSU_HEADER_SIZE;
}

/* ------------------------------------------------------------------ */
/* HELLO / HELLO_ACK                                                  */
/* ------------------------------------------------------------------ */

#define WSU_NAME_LEN 16

typedef struct WsuHello {
    uint8_t role;  /* WsuRole                                           */
    uint8_t flags; /* reserved, 0                                       */
    char name[WSU_NAME_LEN]; /* NUL-padded UTF-8 display name           */
} WsuHello;

#define WSU_HELLO_PAYLOAD_SIZE (2 + WSU_NAME_LEN)

static inline int wsu_pack_hello(uint8_t *buf, size_t cap, uint16_t seq,
                                 const WsuHello *h) {
    if (cap < WSU_HEADER_SIZE + WSU_HELLO_PAYLOAD_SIZE) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_HELLO, seq);
    buf[off++] = h->role;
    buf[off++] = h->flags;
    memcpy(buf + off, h->name, WSU_NAME_LEN);
    return off + WSU_NAME_LEN;
}

/* Payload parsers take the buffer positioned AFTER the common header and
 * the remaining length. They return bytes consumed or -1. */
static inline int wsu_parse_hello(const uint8_t *p, size_t len, WsuHello *h) {
    if (len < WSU_HELLO_PAYLOAD_SIZE) return -1;
    h->role = p[0];
    h->flags = p[1];
    memcpy(h->name, p + 2, WSU_NAME_LEN);
    h->name[WSU_NAME_LEN - 1] = '\0';
    return WSU_HELLO_PAYLOAD_SIZE;
}

typedef struct WsuHelloAck {
    uint8_t status; /* WsuHelloStatus                                   */
    uint8_t slot;   /* assigned player slot 0..3, WSU_NO_SLOT if none   */
} WsuHelloAck;

#define WSU_HELLO_ACK_PAYLOAD_SIZE 2

static inline int wsu_pack_hello_ack(uint8_t *buf, size_t cap, uint16_t seq,
                                     const WsuHelloAck *a) {
    if (cap < WSU_HEADER_SIZE + WSU_HELLO_ACK_PAYLOAD_SIZE) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_HELLO_ACK, seq);
    buf[off++] = a->status;
    buf[off++] = a->slot;
    return off;
}

static inline int wsu_parse_hello_ack(const uint8_t *p, size_t len,
                                      WsuHelloAck *a) {
    if (len < WSU_HELLO_ACK_PAYLOAD_SIZE) return -1;
    a->status = p[0];
    a->slot = p[1];
    return WSU_HELLO_ACK_PAYLOAD_SIZE;
}

/* ------------------------------------------------------------------ */
/* CONFIG                                                             */
/* ------------------------------------------------------------------ */

typedef struct WsuConfig {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t videoCodec; /* WsuVideoCodec                                */
    uint8_t quality;    /* codec-specific, e.g. JPEG quality 1..100     */
    uint8_t audioCodec; /* WsuAudioCodec                                */
    uint16_t audioRateHz;
    uint8_t audioChannels;
    uint8_t flags; /* reserved, 0                                       */
} WsuConfig;

#define WSU_CONFIG_PAYLOAD_SIZE 10

static inline int wsu_pack_config(uint8_t *buf, size_t cap, uint16_t seq,
                                  const WsuConfig *c) {
    if (cap < WSU_HEADER_SIZE + WSU_CONFIG_PAYLOAD_SIZE) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_CONFIG, seq);
    wsu_put_u16(buf + off, c->width);
    wsu_put_u16(buf + off + 2, c->height);
    buf[off + 4] = c->fps;
    buf[off + 5] = c->videoCodec;
    buf[off + 6] = c->quality;
    buf[off + 7] = c->audioCodec;
    wsu_put_u16(buf + off + 8, c->audioRateHz);
    /* audioChannels/flags ride in the last two bytes */
    return off + WSU_CONFIG_PAYLOAD_SIZE;
}

static inline int wsu_parse_config(const uint8_t *p, size_t len,
                                   WsuConfig *c) {
    if (len < WSU_CONFIG_PAYLOAD_SIZE) return -1;
    c->width = wsu_get_u16(p);
    c->height = wsu_get_u16(p + 2);
    c->fps = p[4];
    c->videoCodec = p[5];
    c->quality = p[6];
    c->audioCodec = p[7];
    c->audioRateHz = wsu_get_u16(p + 8);
    c->audioChannels = 0;
    c->flags = 0;
    return WSU_CONFIG_PAYLOAD_SIZE;
}

/* ------------------------------------------------------------------ */
/* VIDEO                                                              */
/* ------------------------------------------------------------------ */

typedef struct WsuVideoHeader {
    uint32_t frameId;
    uint32_t timestampMs;
    uint8_t flags; /* WSU_VIDEO_FLAG_*                                  */
    uint16_t sliceIndex;
    uint16_t sliceCount;
    uint16_t payloadLen;
} WsuVideoHeader;

#define WSU_VIDEO_HEADER_PAYLOAD_SIZE 15

/* Packs a video slice (header + payload copy). Returns total datagram
 * size or -1. */
static inline int wsu_pack_video(uint8_t *buf, size_t cap, uint16_t seq,
                                 const WsuVideoHeader *v,
                                 const uint8_t *payload) {
    size_t need =
        (size_t)WSU_HEADER_SIZE + WSU_VIDEO_HEADER_PAYLOAD_SIZE + v->payloadLen;
    if (cap < need || v->payloadLen > WSU_MAX_SLICE_PAYLOAD) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_VIDEO, seq);
    wsu_put_u32(buf + off, v->frameId);
    wsu_put_u32(buf + off + 4, v->timestampMs);
    buf[off + 8] = v->flags;
    wsu_put_u16(buf + off + 9, v->sliceIndex);
    wsu_put_u16(buf + off + 11, v->sliceCount);
    wsu_put_u16(buf + off + 13, v->payloadLen);
    memcpy(buf + off + WSU_VIDEO_HEADER_PAYLOAD_SIZE, payload, v->payloadLen);
    return (int)need;
}

/* Parses a video slice header; *payload points into the caller's buffer.
 * Returns bytes consumed (header + payload) or -1. */
static inline int wsu_parse_video(const uint8_t *p, size_t len,
                                  WsuVideoHeader *v,
                                  const uint8_t **payload) {
    if (len < WSU_VIDEO_HEADER_PAYLOAD_SIZE) return -1;
    v->frameId = wsu_get_u32(p);
    v->timestampMs = wsu_get_u32(p + 4);
    v->flags = p[8];
    v->sliceIndex = wsu_get_u16(p + 9);
    v->sliceCount = wsu_get_u16(p + 11);
    v->payloadLen = wsu_get_u16(p + 13);
    if (v->payloadLen > WSU_MAX_SLICE_PAYLOAD ||
        len < (size_t)WSU_VIDEO_HEADER_PAYLOAD_SIZE + v->payloadLen ||
        v->sliceCount == 0 || v->sliceIndex >= v->sliceCount) {
        return -1;
    }
    *payload = p + WSU_VIDEO_HEADER_PAYLOAD_SIZE;
    return WSU_VIDEO_HEADER_PAYLOAD_SIZE + v->payloadLen;
}

/* Number of slices needed for a frame of `frameBytes`. */
static inline uint16_t wsu_video_slice_count(uint32_t frameBytes) {
    if (frameBytes == 0) return 1;
    return (uint16_t)((frameBytes + WSU_MAX_SLICE_PAYLOAD - 1) /
                      WSU_MAX_SLICE_PAYLOAD);
}

/* ------------------------------------------------------------------ */
/* AUDIO                                                              */
/* ------------------------------------------------------------------ */

typedef struct WsuAudioHeader {
    uint32_t timestampMs;
    uint16_t payloadLen;
} WsuAudioHeader;

#define WSU_AUDIO_HEADER_PAYLOAD_SIZE 6

static inline int wsu_pack_audio(uint8_t *buf, size_t cap, uint16_t seq,
                                 const WsuAudioHeader *a,
                                 const uint8_t *payload) {
    size_t need =
        (size_t)WSU_HEADER_SIZE + WSU_AUDIO_HEADER_PAYLOAD_SIZE + a->payloadLen;
    if (cap < need || a->payloadLen > WSU_MAX_SLICE_PAYLOAD) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_AUDIO, seq);
    wsu_put_u32(buf + off, a->timestampMs);
    wsu_put_u16(buf + off + 4, a->payloadLen);
    memcpy(buf + off + WSU_AUDIO_HEADER_PAYLOAD_SIZE, payload, a->payloadLen);
    return (int)need;
}

static inline int wsu_parse_audio(const uint8_t *p, size_t len,
                                  WsuAudioHeader *a,
                                  const uint8_t **payload) {
    if (len < WSU_AUDIO_HEADER_PAYLOAD_SIZE) return -1;
    a->timestampMs = wsu_get_u32(p);
    a->payloadLen = wsu_get_u16(p + 4);
    if (a->payloadLen > WSU_MAX_SLICE_PAYLOAD ||
        len < (size_t)WSU_AUDIO_HEADER_PAYLOAD_SIZE + a->payloadLen) {
        return -1;
    }
    *payload = p + WSU_AUDIO_HEADER_PAYLOAD_SIZE;
    return WSU_AUDIO_HEADER_PAYLOAD_SIZE + a->payloadLen;
}

/* ------------------------------------------------------------------ */
/* INPUT / INPUT_BUNDLE                                               */
/* ------------------------------------------------------------------ */

/* Full state of one controller. Sent at a fixed high rate; every packet
 * carries the complete state (latest-state-wins). */
typedef struct WsuInputState {
    uint8_t slot;  /* 0..3                                              */
    uint8_t flags; /* WSU_INPUT_FLAG_*                                  */
    uint32_t buttons; /* WsuButtons bitfield                            */
    int16_t lx, ly; /* left stick, -32767..32767, +x right +y up        */
    int16_t rx, ry; /* right stick                                      */
    uint8_t lt, rt; /* analog triggers 0..255 (GameCube pads)           */
    uint16_t touchX, touchY; /* 0..4095 normalized, WSU_TOUCH_NONE      */
    int16_t gyroPitch, gyroYaw, gyroRoll; /* deg/s, scaled x16          */
} WsuInputState;

#define WSU_INPUT_STATE_SIZE 26

static inline void wsu_input_state_init(WsuInputState *s, uint8_t slot) {
    memset(s, 0, sizeof(*s));
    s->slot = slot;
    s->touchX = WSU_TOUCH_NONE;
    s->touchY = WSU_TOUCH_NONE;
}

/* Serializes one input body at `p` (no packet header). Returns
 * WSU_INPUT_STATE_SIZE. `p` must hold at least that many bytes. */
static inline int wsu_write_input_state(uint8_t *p, const WsuInputState *s) {
    p[0] = s->slot;
    p[1] = s->flags;
    wsu_put_u32(p + 2, s->buttons);
    wsu_put_i16(p + 6, s->lx);
    wsu_put_i16(p + 8, s->ly);
    wsu_put_i16(p + 10, s->rx);
    wsu_put_i16(p + 12, s->ry);
    p[14] = s->lt;
    p[15] = s->rt;
    wsu_put_u16(p + 16, s->touchX);
    wsu_put_u16(p + 18, s->touchY);
    wsu_put_i16(p + 20, s->gyroPitch);
    wsu_put_i16(p + 22, s->gyroYaw);
    wsu_put_i16(p + 24, s->gyroRoll);
    return WSU_INPUT_STATE_SIZE;
}

static inline int wsu_read_input_state(const uint8_t *p, size_t len,
                                       WsuInputState *s) {
    if (len < WSU_INPUT_STATE_SIZE) return -1;
    s->slot = p[0];
    s->flags = p[1];
    s->buttons = wsu_get_u32(p + 2);
    s->lx = wsu_get_i16(p + 6);
    s->ly = wsu_get_i16(p + 8);
    s->rx = wsu_get_i16(p + 10);
    s->ry = wsu_get_i16(p + 12);
    s->lt = p[14];
    s->rt = p[15];
    s->touchX = wsu_get_u16(p + 16);
    s->touchY = wsu_get_u16(p + 18);
    s->gyroPitch = wsu_get_i16(p + 20);
    s->gyroYaw = wsu_get_i16(p + 22);
    s->gyroRoll = wsu_get_i16(p + 24);
    return WSU_INPUT_STATE_SIZE;
}

/* INPUT: client → host. timestampMs + one body. */
static inline int wsu_pack_input(uint8_t *buf, size_t cap, uint16_t seq,
                                 uint32_t timestampMs,
                                 const WsuInputState *s) {
    if (cap < WSU_HEADER_SIZE + 4 + WSU_INPUT_STATE_SIZE) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_INPUT, seq);
    wsu_put_u32(buf + off, timestampMs);
    off += 4;
    off += wsu_write_input_state(buf + off, s);
    return off;
}

static inline int wsu_parse_input(const uint8_t *p, size_t len,
                                  uint32_t *timestampMs, WsuInputState *s) {
    if (len < 4 + WSU_INPUT_STATE_SIZE) return -1;
    *timestampMs = wsu_get_u32(p);
    if (wsu_read_input_state(p + 4, len - 4, s) < 0) return -1;
    return 4 + WSU_INPUT_STATE_SIZE;
}

/* INPUT_BUNDLE: host → console. timestampMs + count + count bodies. */
static inline int wsu_pack_input_bundle(uint8_t *buf, size_t cap,
                                        uint16_t seq, uint32_t timestampMs,
                                        const WsuInputState *states,
                                        uint8_t count) {
    size_t need = (size_t)WSU_HEADER_SIZE + 5 +
                  (size_t)count * WSU_INPUT_STATE_SIZE;
    if (count > WSU_MAX_PLAYERS || cap < need) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_INPUT_BUNDLE, seq);
    wsu_put_u32(buf + off, timestampMs);
    buf[off + 4] = count;
    off += 5;
    for (uint8_t i = 0; i < count; i++) {
        off += wsu_write_input_state(buf + off, &states[i]);
    }
    return off;
}

/* `states` must hold WSU_MAX_PLAYERS entries. Returns bytes consumed and
 * sets *count, or -1. */
static inline int wsu_parse_input_bundle(const uint8_t *p, size_t len,
                                         uint32_t *timestampMs,
                                         WsuInputState *states,
                                         uint8_t *count) {
    if (len < 5) return -1;
    *timestampMs = wsu_get_u32(p);
    uint8_t n = p[4];
    if (n > WSU_MAX_PLAYERS ||
        len < 5 + (size_t)n * WSU_INPUT_STATE_SIZE) {
        return -1;
    }
    for (uint8_t i = 0; i < n; i++) {
        wsu_read_input_state(p + 5 + (size_t)i * WSU_INPUT_STATE_SIZE,
                             WSU_INPUT_STATE_SIZE, &states[i]);
    }
    *count = n;
    return 5 + (int)n * WSU_INPUT_STATE_SIZE;
}

/* ------------------------------------------------------------------ */
/* RUMBLE / PING / PONG / BYE                                         */
/* ------------------------------------------------------------------ */

static inline int wsu_pack_rumble(uint8_t *buf, size_t cap, uint16_t seq,
                                  uint8_t slot, uint8_t intensity) {
    if (cap < WSU_HEADER_SIZE + 2) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_RUMBLE, seq);
    buf[off++] = slot;
    buf[off++] = intensity;
    return off;
}

static inline int wsu_parse_rumble(const uint8_t *p, size_t len,
                                   uint8_t *slot, uint8_t *intensity) {
    if (len < 2) return -1;
    *slot = p[0];
    *intensity = p[1];
    return 2;
}

static inline int wsu_pack_ping(uint8_t *buf, size_t cap, uint16_t seq,
                                uint32_t timestampMs) {
    if (cap < WSU_HEADER_SIZE + 4) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_PING, seq);
    wsu_put_u32(buf + off, timestampMs);
    return off + 4;
}

static inline int wsu_pack_pong(uint8_t *buf, size_t cap, uint16_t seq,
                                uint32_t echoTimestampMs) {
    if (cap < WSU_HEADER_SIZE + 4) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_PONG, seq);
    wsu_put_u32(buf + off, echoTimestampMs);
    return off + 4;
}

static inline int wsu_parse_timestamp(const uint8_t *p, size_t len,
                                      uint32_t *timestampMs) {
    if (len < 4) return -1;
    *timestampMs = wsu_get_u32(p);
    return 4;
}

static inline int wsu_pack_bye(uint8_t *buf, size_t cap, uint16_t seq,
                               uint8_t reason) {
    if (cap < WSU_HEADER_SIZE + 1) return -1;
    int off = wsu_pack_header(buf, WSU_PKT_BYE, seq);
    buf[off++] = reason;
    return off;
}

static inline int wsu_pack_keyframe_req(uint8_t *buf, size_t cap,
                                        uint16_t seq) {
    if (cap < WSU_HEADER_SIZE) return -1;
    return wsu_pack_header(buf, WSU_PKT_KEYFRAME_REQ, seq);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WSU_PROTOCOL_H */
