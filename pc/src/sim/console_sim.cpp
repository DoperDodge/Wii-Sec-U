// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "sim/console_sim.h"

#include <cstring>
#include <vector>

#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {
constexpr const char *kTag = "console-sim";
constexpr unsigned kRecvTimeoutMs = 100;
constexpr uint32_t kConfigIntervalMs = 2000;
} // namespace

ConsoleSim::ConsoleSim(ConsoleSimOptions options) : options_(options) {
    for (uint8_t i = 0; i < WSU_MAX_PLAYERS; i++) {
        wsu_input_state_init(&lastInput_[i], i);
    }
}

ConsoleSim::~ConsoleSim() { stop(); }

bool ConsoleSim::start() {
    if (running_.load()) return true;
    if (!inputSocket_.open(options_.inputPort) ||
        !streamSocket_.open(options_.streamPort)) {
        logError(kTag, "failed to bind ports %u/%u", options_.inputPort,
                 options_.streamPort);
        return false;
    }
    inputSocket_.setRecvTimeout(kRecvTimeoutMs);
    // The stream loop paces frames between receives, so its blocking
    // window must stay well under the frame interval.
    streamSocket_.setRecvTimeout(5);
    inputPort_.store(inputSocket_.boundPort());
    streamPort_.store(streamSocket_.boundPort());
    running_.store(true);
    inputThread_ = std::thread(&ConsoleSim::inputLoop, this);
    streamThread_ = std::thread(&ConsoleSim::streamLoop, this);
    logInfo(kTag, "simulated console up: input UDP %u, stream UDP %u",
            inputPort_.load(), streamPort_.load());
    return true;
}

void ConsoleSim::stop() {
    if (!running_.exchange(false)) return;
    if (inputThread_.joinable()) inputThread_.join();
    if (streamThread_.joinable()) streamThread_.join();
    inputSocket_.close();
    streamSocket_.close();
}

bool ConsoleSim::lastInput(uint8_t slot, WsuInputState &out) const {
    if (slot >= WSU_MAX_PLAYERS) return false;
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (!haveInput_[slot]) return false;
    out = lastInput_[slot];
    return true;
}

void ConsoleSim::inputLoop() {
    uint8_t buf[WSU_MAX_PACKET];
    uint16_t seq = 0;

    while (running_.load()) {
        size_t len = 0;
        Endpoint from;
        RecvResult r = inputSocket_.recvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::Error) {
            if (running_.load()) sleepMs(50);
            continue;
        }
        if (r == RecvResult::Timeout) continue;

        WsuHeader hdr;
        if (wsu_parse_header(buf, len, &hdr) < 0) continue;
        const uint8_t *p = buf + WSU_HEADER_SIZE;
        size_t plen = len - WSU_HEADER_SIZE;

        switch (hdr.type) {
        case WSU_PKT_HELLO: {
            WsuHelloAck ack{};
            ack.status = hdr.version == WSU_PROTO_VERSION
                             ? WSU_HELLO_OK
                             : WSU_HELLO_VERSION_MISMATCH;
            ack.slot = WSU_NO_SLOT;
            int n = wsu_pack_hello_ack(buf, sizeof(buf), seq++, &ack);
            if (n > 0) inputSocket_.sendTo(from, buf, n);
            break;
        }
        case WSU_PKT_INPUT_BUNDLE: {
            uint32_t ts;
            WsuInputState states[WSU_MAX_PLAYERS];
            uint8_t count = 0;
            if (wsu_parse_input_bundle(p, plen, &ts, states, &count) < 0) {
                break;
            }
            bundles_++;
            std::lock_guard<std::mutex> lock(inputMutex_);
            for (uint8_t i = 0; i < count; i++) {
                uint8_t slot = states[i].slot;
                if (slot >= WSU_MAX_PLAYERS) continue;
                if (options_.logInput &&
                    (states[i].buttons != lastInput_[slot].buttons)) {
                    logInfo(kTag, "P%u buttons=0x%08x lx=%d ly=%d", slot + 1,
                            states[i].buttons, states[i].lx, states[i].ly);
                }
                lastInput_[slot] = states[i];
                haveInput_[slot] = true;
            }
            break;
        }
        case WSU_PKT_PING: {
            uint32_t ts;
            if (wsu_parse_timestamp(p, plen, &ts) == 4) {
                uint8_t out[WSU_HEADER_SIZE + 4];
                int n = wsu_pack_pong(out, sizeof(out), seq++, ts);
                if (n > 0) inputSocket_.sendTo(from, out, n);
            }
            break;
        }
        default:
            break;
        }
    }
}

void ConsoleSim::buildFrame(std::vector<uint8_t> &rgb,
                            uint32_t frameId) const {
    const unsigned w = options_.width;
    const unsigned h = options_.height;
    rgb.resize(static_cast<size_t>(w) * h * 3);
    // Horizontal gradient plus a vertical bar that advances one column per
    // frame — visibly moving, and byte-exact reproducible from frameId.
    unsigned barX = frameId % w;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            size_t i = (static_cast<size_t>(y) * w + x) * 3;
            rgb[i] = static_cast<uint8_t>((x * 255) / (w ? w : 1));
            rgb[i + 1] = static_cast<uint8_t>((y * 255) / (h ? h : 1));
            rgb[i + 2] = (x == barX) ? 255 : static_cast<uint8_t>(frameId);
        }
    }
}

void ConsoleSim::streamLoop() {
    uint8_t buf[WSU_MAX_PACKET];
    std::vector<uint8_t> frame;
    uint16_t seq = 0;
    uint32_t frameId = 0;
    Endpoint host;
    uint32_t lastHostSeen = 0;
    uint32_t lastFrame = 0;
    uint32_t lastConfig = 0;
    const uint32_t frameIntervalMs =
        options_.fps > 0 ? 1000 / options_.fps : 66;

    WsuConfig config{};
    config.width = options_.width;
    config.height = options_.height;
    config.fps = options_.fps;
    config.videoCodec = WSU_CODEC_RAWRGB;
    config.quality = 100;
    config.audioCodec = WSU_AUDIO_NONE;

    while (running_.load()) {
        uint32_t now = nowMs();

        if (hostKnown_.load() && lastHostSeen != 0 &&
            ageMs(now, lastHostSeen) > WSU_PEER_TIMEOUT_MS) {
            logWarn(kTag, "host lost, pausing stream");
            hostKnown_.store(false);
        }

        if (hostKnown_.load()) {
            if (ageMs(now, lastConfig) >= kConfigIntervalMs) {
                int n = wsu_pack_config(buf, sizeof(buf), seq++, &config);
                if (n > 0) streamSocket_.sendTo(host, buf, n);
                lastConfig = now;
            }

            if (ageMs(now, lastFrame) >= frameIntervalMs) {
                buildFrame(frame, frameId);
                uint16_t slices =
                    wsu_video_slice_count(static_cast<uint32_t>(frame.size()));
                size_t off = 0;
                for (uint16_t s = 0; s < slices; s++) {
                    WsuVideoHeader v{};
                    v.frameId = frameId;
                    v.timestampMs = now;
                    v.flags = WSU_VIDEO_FLAG_KEYFRAME; // raw frames are
                                                       // self-contained
                    v.sliceIndex = s;
                    v.sliceCount = slices;
                    size_t remain = frame.size() - off;
                    v.payloadLen = static_cast<uint16_t>(
                        remain < WSU_MAX_SLICE_PAYLOAD ? remain
                                                       : WSU_MAX_SLICE_PAYLOAD);
                    int n = wsu_pack_video(buf, sizeof(buf), seq++, &v,
                                           frame.data() + off);
                    if (n > 0) streamSocket_.sendTo(host, buf, n);
                    off += v.payloadLen;
                }
                frameId++;
                framesSent_.store(frameId);
                lastFrame = now;
            }
        }

        size_t len = 0;
        Endpoint from;
        RecvResult r = streamSocket_.recvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::Error) {
            if (running_.load()) sleepMs(50);
            continue;
        }
        if (r == RecvResult::Timeout) continue;

        WsuHeader hdr;
        if (wsu_parse_header(buf, len, &hdr) < 0) continue;
        const uint8_t *p = buf + WSU_HEADER_SIZE;
        size_t plen = len - WSU_HEADER_SIZE;
        now = nowMs();

        switch (hdr.type) {
        case WSU_PKT_HELLO: {
            WsuHelloAck ack{};
            ack.status = hdr.version == WSU_PROTO_VERSION
                             ? WSU_HELLO_OK
                             : WSU_HELLO_VERSION_MISMATCH;
            ack.slot = WSU_NO_SLOT;
            int n = wsu_pack_hello_ack(buf, sizeof(buf), seq++, &ack);
            if (n > 0) streamSocket_.sendTo(from, buf, n);
            if (ack.status == WSU_HELLO_OK) {
                host = from;
                lastHostSeen = now;
                lastConfig = 0; // push config immediately
                if (!hostKnown_.exchange(true)) {
                    logInfo(kTag, "host connected from %s",
                            from.toString().c_str());
                }
            }
            break;
        }
        case WSU_PKT_PING: {
            uint32_t ts;
            if (wsu_parse_timestamp(p, plen, &ts) == 4) {
                uint8_t out[WSU_HEADER_SIZE + 4];
                int n = wsu_pack_pong(out, sizeof(out), seq++, ts);
                if (n > 0) streamSocket_.sendTo(from, out, n);
                if (from == host) lastHostSeen = now;
            }
            break;
        }
        case WSU_PKT_BYE:
            if (from == host) {
                logInfo(kTag, "host said goodbye");
                hostKnown_.store(false);
            }
            break;
        default:
            break;
        }
    }
}

} // namespace wsu
