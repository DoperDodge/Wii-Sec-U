// Wire-protocol round-trip and known-bytes tests.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <cstring>

#include "test_common.h"
#include "wsu_protocol.h"

static void testHeader() {
    uint8_t buf[64];
    int n = wsu_pack_header(buf, WSU_PKT_PING, 0xBEEF);
    CHECK_EQ(n, WSU_HEADER_SIZE);

    // Known bytes: magic "WSU1", version, type, seq — big-endian.
    CHECK_EQ(buf[0], 'W');
    CHECK_EQ(buf[1], 'S');
    CHECK_EQ(buf[2], 'U');
    CHECK_EQ(buf[3], '1');
    CHECK_EQ(buf[4], WSU_PROTO_VERSION);
    CHECK_EQ(buf[5], WSU_PKT_PING);
    CHECK_EQ(buf[6], 0xBE);
    CHECK_EQ(buf[7], 0xEF);

    WsuHeader hdr;
    CHECK_EQ(wsu_parse_header(buf, n, &hdr), WSU_HEADER_SIZE);
    CHECK_EQ(hdr.version, WSU_PROTO_VERSION);
    CHECK_EQ(hdr.type, WSU_PKT_PING);
    CHECK_EQ(hdr.seq, 0xBEEF);

    // Truncated and corrupted headers must be rejected.
    CHECK_EQ(wsu_parse_header(buf, 7, &hdr), -1);
    buf[0] = 'X';
    CHECK_EQ(wsu_parse_header(buf, n, &hdr), -1);
}

static void testHello() {
    uint8_t buf[64];
    WsuHello hello{};
    hello.role = WSU_ROLE_CLIENT;
    std::strncpy(hello.name, "mario", WSU_NAME_LEN - 1);
    int n = wsu_pack_hello(buf, sizeof(buf), 1, &hello);
    CHECK(n > 0);

    WsuHeader hdr;
    wsu_parse_header(buf, n, &hdr);
    CHECK_EQ(hdr.type, WSU_PKT_HELLO);

    WsuHello out;
    CHECK(wsu_parse_hello(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE, &out) >
          0);
    CHECK_EQ(out.role, WSU_ROLE_CLIENT);
    CHECK_EQ(std::strcmp(out.name, "mario"), 0);

    WsuHelloAck ack{};
    ack.status = WSU_HELLO_OK;
    ack.slot = 2;
    n = wsu_pack_hello_ack(buf, sizeof(buf), 2, &ack);
    CHECK(n > 0);
    WsuHelloAck ackOut;
    CHECK(wsu_parse_hello_ack(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE,
                              &ackOut) > 0);
    CHECK_EQ(ackOut.status, WSU_HELLO_OK);
    CHECK_EQ(ackOut.slot, 2);
}

static void testConfig() {
    uint8_t buf[64];
    WsuConfig c{};
    c.width = 428;
    c.height = 240;
    c.fps = 20;
    c.videoCodec = WSU_CODEC_MJPEG;
    c.quality = 60;
    c.audioCodec = WSU_AUDIO_PCM16;
    c.audioRateHz = 48000;
    int n = wsu_pack_config(buf, sizeof(buf), 3, &c);
    CHECK(n > 0);

    WsuConfig out;
    CHECK(wsu_parse_config(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE,
                           &out) > 0);
    CHECK_EQ(out.width, 428);
    CHECK_EQ(out.height, 240);
    CHECK_EQ(out.fps, 20);
    CHECK_EQ(out.videoCodec, WSU_CODEC_MJPEG);
    CHECK_EQ(out.quality, 60);
    CHECK_EQ(out.audioCodec, WSU_AUDIO_PCM16);
    CHECK_EQ(out.audioRateHz, 48000);
}

static void testVideo() {
    uint8_t buf[WSU_MAX_PACKET];
    uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = static_cast<uint8_t>(i * 7);
    }

    WsuVideoHeader v{};
    v.frameId = 0x01020304;
    v.timestampMs = 0xA1B2C3D4;
    v.flags = WSU_VIDEO_FLAG_KEYFRAME;
    v.sliceIndex = 2;
    v.sliceCount = 5;
    v.payloadLen = sizeof(payload);
    int n = wsu_pack_video(buf, sizeof(buf), 4, &v, payload);
    CHECK_EQ(n, WSU_HEADER_SIZE + WSU_VIDEO_HEADER_PAYLOAD_SIZE + 300);

    WsuVideoHeader out;
    const uint8_t *outPayload = nullptr;
    int consumed = wsu_parse_video(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE,
                                   &out, &outPayload);
    CHECK(consumed > 0);
    CHECK_EQ(out.frameId, 0x01020304u);
    CHECK_EQ(out.timestampMs, 0xA1B2C3D4u);
    CHECK_EQ(out.flags, WSU_VIDEO_FLAG_KEYFRAME);
    CHECK_EQ(out.sliceIndex, 2);
    CHECK_EQ(out.sliceCount, 5);
    CHECK_EQ(out.payloadLen, 300);
    CHECK_EQ(std::memcmp(outPayload, payload, sizeof(payload)), 0);

    // Slice index out of range must be rejected.
    v.sliceIndex = 5;
    n = wsu_pack_video(buf, sizeof(buf), 5, &v, payload);
    CHECK(wsu_parse_video(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE, &out,
                          &outPayload) < 0);

    // Truncated payload must be rejected.
    v.sliceIndex = 0;
    n = wsu_pack_video(buf, sizeof(buf), 6, &v, payload);
    CHECK(wsu_parse_video(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE - 10,
                          &out, &outPayload) < 0);

    CHECK_EQ(wsu_video_slice_count(0), 1);
    CHECK_EQ(wsu_video_slice_count(1), 1);
    CHECK_EQ(wsu_video_slice_count(WSU_MAX_SLICE_PAYLOAD), 1);
    CHECK_EQ(wsu_video_slice_count(WSU_MAX_SLICE_PAYLOAD + 1), 2);
}

static void testInput() {
    uint8_t buf[128];
    WsuInputState s;
    wsu_input_state_init(&s, 1);
    CHECK_EQ(s.touchX, WSU_TOUCH_NONE);
    s.buttons = WSU_BTN_A | WSU_BTN_ZL | WSU_BTN_STICK_R;
    s.lx = -32767;
    s.ly = 32767;
    s.rx = 12345;
    s.ry = -12345;
    s.lt = 200;
    s.rt = 55;
    s.flags = WSU_INPUT_FLAG_TOUCHING;
    s.touchX = 2048;
    s.touchY = 1024;
    s.gyroPitch = -100;
    s.gyroYaw = 200;
    s.gyroRoll = -300;

    int n = wsu_pack_input(buf, sizeof(buf), 7, 0x11223344, &s);
    CHECK(n > 0);
    uint32_t ts;
    WsuInputState out;
    CHECK(wsu_parse_input(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE, &ts,
                          &out) > 0);
    CHECK_EQ(ts, 0x11223344u);
    CHECK_EQ(out.slot, 1);
    CHECK_EQ(out.buttons, s.buttons);
    CHECK_EQ(out.lx, -32767);
    CHECK_EQ(out.ly, 32767);
    CHECK_EQ(out.rx, 12345);
    CHECK_EQ(out.ry, -12345);
    CHECK_EQ(out.lt, 200);
    CHECK_EQ(out.rt, 55);
    CHECK_EQ(out.touchX, 2048);
    CHECK_EQ(out.touchY, 1024);
    CHECK_EQ(out.gyroPitch, -100);
    CHECK_EQ(out.gyroYaw, 200);
    CHECK_EQ(out.gyroRoll, -300);
}

static void testInputBundle() {
    uint8_t buf[256];
    WsuInputState states[3];
    for (uint8_t i = 0; i < 3; i++) {
        wsu_input_state_init(&states[i], i);
        states[i].buttons = WSU_BTN_A << i;
        states[i].lx = static_cast<int16_t>(100 * (i + 1));
    }

    int n = wsu_pack_input_bundle(buf, sizeof(buf), 8, 999, states, 3);
    CHECK(n > 0);

    uint32_t ts;
    WsuInputState out[WSU_MAX_PLAYERS];
    uint8_t count = 0;
    CHECK(wsu_parse_input_bundle(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE,
                                 &ts, out, &count) > 0);
    CHECK_EQ(ts, 999u);
    CHECK_EQ(count, 3);
    for (uint8_t i = 0; i < 3; i++) {
        CHECK_EQ(out[i].slot, i);
        CHECK_EQ(out[i].buttons, static_cast<uint32_t>(WSU_BTN_A << i));
        CHECK_EQ(out[i].lx, 100 * (i + 1));
    }

    // Bundles larger than WSU_MAX_PLAYERS are invalid.
    WsuInputState many[5];
    for (auto &m : many) wsu_input_state_init(&m, 0);
    CHECK(wsu_pack_input_bundle(buf, sizeof(buf), 9, 0, many, 5) < 0);

    // Truncated bundle rejected.
    n = wsu_pack_input_bundle(buf, sizeof(buf), 10, 0, states, 3);
    CHECK(wsu_parse_input_bundle(buf + WSU_HEADER_SIZE,
                                 n - WSU_HEADER_SIZE - 4, &ts, out,
                                 &count) < 0);
}

static void testMisc() {
    uint8_t buf[64];
    int n = wsu_pack_ping(buf, sizeof(buf), 11, 0xCAFEBABE);
    CHECK(n > 0);
    uint32_t ts;
    CHECK_EQ(
        wsu_parse_timestamp(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE, &ts),
        4);
    CHECK_EQ(ts, 0xCAFEBABEu);

    n = wsu_pack_rumble(buf, sizeof(buf), 12, 3, 128);
    CHECK(n > 0);
    uint8_t slot, intensity;
    CHECK(wsu_parse_rumble(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE, &slot,
                           &intensity) > 0);
    CHECK_EQ(slot, 3);
    CHECK_EQ(intensity, 128);

    n = wsu_pack_bye(buf, sizeof(buf), 13, WSU_BYE_TIMEOUT);
    CHECK(n > 0);
    WsuHeader hdr;
    wsu_parse_header(buf, n, &hdr);
    CHECK_EQ(hdr.type, WSU_PKT_BYE);
    CHECK_EQ(buf[WSU_HEADER_SIZE], WSU_BYE_TIMEOUT);

    // Pack into too-small buffers must fail cleanly.
    CHECK(wsu_pack_ping(buf, 4, 14, 0) < 0);
    WsuConfig c{};
    CHECK(wsu_pack_config(buf, 10, 15, &c) < 0);
}

int main() {
    testHeader();
    testHello();
    testConfig();
    testVideo();
    testInput();
    testInputBundle();
    testMisc();
    return testSummary("test_protocol");
}
