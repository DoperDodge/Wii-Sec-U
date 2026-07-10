// wsu-stream network module: one UDP socket to the host PC.
//
// Receives HELLO/PING/BYE from the host; sends HELLO_ACK/PONG/CONFIG,
// VIDEO (from the encoder thread) and AUDIO (drained from audio.cpp's
// ring buffer). The host address is learned from its HELLO and expires
// after WSU_PEER_TIMEOUT_MS of silence — the host pings once per second.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>

#include <coreinit/mutex.h>

#include "stream_state.h"
#include "wsu_protocol.h"
#include "wsu_wiiu.h"

namespace wsu {

namespace {

constexpr uint32_t kNetStackSize = 64 * 1024;
constexpr int32_t kNetPriority = 22;
constexpr uint32_t kConfigIntervalMs = 2000;
// 256 stereo pairs = 1024 bytes/packet ≈ 5.3 ms at 48 kHz.
constexpr size_t kAudioChunkPairs = 256;

std::atomic<bool> gRunning{false};
std::atomic<bool> gHostActive{false};
int gSock = -1;

OSMutex gSendMutex; // serializes sendto + host endpoint access
sockaddr_in gHostAddr;
uint32_t gLastHostSeenMs = 0;
uint16_t gTxSeq = 0;

PluginThread gThread;

void sendLocked(const uint8_t *buf, int len) {
    if (len <= 0 || gSock < 0) return;
    sendto(gSock, buf, static_cast<size_t>(len), 0,
           reinterpret_cast<const sockaddr *>(&gHostAddr),
           sizeof(gHostAddr));
}

void sendConfig() {
    WsuConfig c{};
    c.width = static_cast<uint16_t>(gConfig.width);
    c.height = static_cast<uint16_t>(gConfig.height);
    c.fps = static_cast<uint8_t>(gConfig.fps);
    c.videoCodec = WSU_CODEC_MJPEG;
    c.quality = static_cast<uint8_t>(gConfig.quality);
    c.audioCodec = gConfig.audio ? WSU_AUDIO_PCM16 : WSU_AUDIO_NONE;
    c.audioRateHz = 48000;
    c.audioChannels = 2;

    uint8_t buf[WSU_HEADER_SIZE + WSU_CONFIG_PAYLOAD_SIZE];
    int n = wsu_pack_config(buf, sizeof(buf), gTxSeq++, &c);
    sendLocked(buf, n);
}

void drainAudio() {
    if (!gConfig.audio) return;
    static int16_t samples[kAudioChunkPairs * 2];
    static uint8_t packet[WSU_MAX_PACKET];

    // Bounded per pass so a full ring can't starve the receive loop.
    for (int i = 0; i < 8; i++) {
        size_t pairs = audioRead(samples, kAudioChunkPairs);
        if (pairs < kAudioChunkPairs) break; // send only full chunks

        WsuAudioHeader a{};
        a.timestampMs = nowMs();
        a.payloadLen = static_cast<uint16_t>(pairs * 2 * sizeof(int16_t));

        // PCM16 big-endian on the wire, written byte-wise.
        uint8_t payload[kAudioChunkPairs * 2 * sizeof(int16_t)];
        for (size_t sIdx = 0; sIdx < pairs * 2; sIdx++) {
            wsu_put_i16(payload + sIdx * 2, samples[sIdx]);
        }
        int n = wsu_pack_audio(packet, sizeof(packet), gTxSeq++, &a,
                               payload);
        OSLockMutex(&gSendMutex);
        sendLocked(packet, n);
        OSUnlockMutex(&gSendMutex);
    }
}

int netThread(int, const char **) {
    uint8_t buf[WSU_MAX_PACKET];
    sockaddr_in from;
    socklen_t fromLen;
    uint32_t lastConfigMs = 0;

    WSU_LOG("stream: listening on UDP %d", gConfig.port);

    while (gRunning.load()) {
        uint32_t now = nowMs();

        if (gHostActive.load()) {
            if (ageMs(now, gLastHostSeenMs) > WSU_PEER_TIMEOUT_MS) {
                WSU_LOG("stream: host lost (timeout), pausing");
                gHostActive.store(false);
            } else if (ageMs(now, lastConfigMs) >= kConfigIntervalMs) {
                OSLockMutex(&gSendMutex);
                sendConfig();
                OSUnlockMutex(&gSendMutex);
                lastConfigMs = now;
            }
            drainAudio();
        }

        audioEnsureRegistered();

        fromLen = sizeof(from);
        int len = recvfrom(gSock, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (len <= 0) {
            sleepMs(2);
            continue;
        }

        WsuHeader hdr;
        if (wsu_parse_header(buf, static_cast<size_t>(len), &hdr) < 0) {
            continue;
        }
        const uint8_t *p = buf + WSU_HEADER_SIZE;
        size_t plen = static_cast<size_t>(len) - WSU_HEADER_SIZE;
        now = nowMs();

        switch (hdr.type) {
        case WSU_PKT_HELLO: {
            WsuHelloAck ack{};
            ack.status = hdr.version == WSU_PROTO_VERSION
                             ? WSU_HELLO_OK
                             : WSU_HELLO_VERSION_MISMATCH;
            ack.slot = WSU_NO_SLOT;
            uint8_t out[WSU_HEADER_SIZE + WSU_HELLO_ACK_PAYLOAD_SIZE];
            int n = wsu_pack_hello_ack(out, sizeof(out), gTxSeq++, &ack);
            OSLockMutex(&gSendMutex);
            gHostAddr = from;
            sendLocked(out, n);
            if (ack.status == WSU_HELLO_OK) {
                gLastHostSeenMs = now;
                sendConfig();
                lastConfigMs = now;
                if (!gHostActive.exchange(true)) {
                    WSU_LOG("stream: host connected, streaming %dx%d@%d q%d",
                            gConfig.width, gConfig.height, gConfig.fps,
                            gConfig.quality);
                }
            }
            OSUnlockMutex(&gSendMutex);
            break;
        }
        case WSU_PKT_PING: {
            uint32_t ts;
            if (wsu_parse_timestamp(p, plen, &ts) == 4) {
                uint8_t out[WSU_HEADER_SIZE + 4];
                int n = wsu_pack_pong(out, sizeof(out), gTxSeq++, ts);
                OSLockMutex(&gSendMutex);
                sendLocked(out, n);
                OSUnlockMutex(&gSendMutex);
                gLastHostSeenMs = now;
            }
            break;
        }
        case WSU_PKT_BYE:
            WSU_LOG("stream: host disconnected");
            gHostActive.store(false);
            break;
        default:
            break;
        }
    }

    return 0;
}

} // namespace

bool netHostActive() { return gHostActive.load(); }

void netSendVideoFrame(const uint8_t *data, size_t len, uint32_t timestampMs,
                       bool keyframe) {
    if (!gHostActive.load() || len == 0) return;

    static uint32_t frameId = 0; // only the encoder thread calls this
    frameId++;

    uint16_t slices = wsu_video_slice_count(static_cast<uint32_t>(len));
    uint8_t packet[WSU_MAX_PACKET];
    size_t off = 0;

    for (uint16_t s = 0; s < slices; s++) {
        WsuVideoHeader v{};
        v.frameId = frameId;
        v.timestampMs = timestampMs;
        v.flags = keyframe ? WSU_VIDEO_FLAG_KEYFRAME : 0;
        v.sliceIndex = s;
        v.sliceCount = slices;
        size_t remain = len - off;
        v.payloadLen = static_cast<uint16_t>(
            remain < WSU_MAX_SLICE_PAYLOAD ? remain : WSU_MAX_SLICE_PAYLOAD);

        int n = wsu_pack_video(packet, sizeof(packet), gTxSeq++, &v,
                               data + off);
        OSLockMutex(&gSendMutex);
        sendLocked(packet, n);
        OSUnlockMutex(&gSendMutex);
        off += v.payloadLen;
    }
}

bool netStart() {
    if (gRunning.load()) return true;
    OSInitMutex(&gSendMutex);
    gSock = openUdpSocket(static_cast<uint16_t>(gConfig.port));
    if (gSock < 0) {
        WSU_LOG("stream: failed to open UDP %d", gConfig.port);
        return false;
    }
    gRunning.store(true);
    if (!gThread.start(netThread, kNetStackSize, kNetPriority,
                       OS_THREAD_ATTRIB_AFFINITY_CPU2, "wsu-stream-net")) {
        WSU_LOG("stream: failed to start network thread");
        gRunning.store(false);
        close(gSock);
        gSock = -1;
        return false;
    }
    return true;
}

void netStop() {
    if (!gRunning.exchange(false)) return;
    gThread.join();
    gHostActive.store(false);
    if (gSock >= 0) {
        close(gSock);
        gSock = -1;
    }
}

} // namespace wsu
