// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "host/wiiu_link.h"

#include <cstring>

#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {
constexpr const char *kTag = "wiiu-link";
constexpr unsigned kRecvTimeoutMs = 250;
constexpr uint32_t kHelloIntervalMs = 1000;
constexpr uint32_t kPingIntervalMs = 1000;
} // namespace

WiiULink::WiiULink(Endpoint consoleAddr, uint16_t inputPort,
                   uint16_t streamPort, Callbacks callbacks)
    : consoleBase_(consoleAddr), consoleInputPort_(inputPort),
      consoleStreamPort_(streamPort), callbacks_(std::move(callbacks)) {}

WiiULink::~WiiULink() { stop(); }

bool WiiULink::start() {
    if (running_.load()) return true;
    if (!inputSocket_.open(0) || !streamSocket_.open(0)) {
        logError(kTag, "failed to open sockets");
        return false;
    }
    inputSocket_.setRecvTimeout(kRecvTimeoutMs);
    streamSocket_.setRecvTimeout(kRecvTimeoutMs);
    {
        std::lock_guard<std::mutex> lock(inputEpMutex_);
        inputEp_ = Endpoint{consoleBase_.addr, consoleInputPort_};
    }
    running_.store(true);
    inputThread_ = std::thread(&WiiULink::inputLoop, this);
    streamThread_ = std::thread(&WiiULink::streamLoop, this);
    return true;
}

void WiiULink::stop() {
    if (!running_.exchange(false)) return;
    if (inputThread_.joinable()) inputThread_.join();
    if (streamThread_.joinable()) streamThread_.join();

    uint8_t buf[WSU_HEADER_SIZE + 1];
    int n = wsu_pack_bye(buf, sizeof(buf), seq_++, WSU_BYE_SHUTDOWN);
    if (n > 0) {
        std::lock_guard<std::mutex> lock(inputEpMutex_);
        if (inputUp_.load()) inputSocket_.sendTo(inputEp_, buf, n);
    }
    inputSocket_.close();
    streamSocket_.close();
    inputUp_.store(false);
    streamUp_.store(false);
}

void WiiULink::sendInputBundle(const uint8_t *buf, size_t len) {
    if (!running_.load() || !inputUp_.load()) return;
    Endpoint ep;
    {
        std::lock_guard<std::mutex> lock(inputEpMutex_);
        ep = inputEp_;
    }
    inputSocket_.sendTo(ep, buf, len);
}

void WiiULink::inputLoop() {
    uint8_t buf[WSU_MAX_PACKET];
    uint32_t lastHello = 0;
    uint32_t lastPing = 0;
    uint32_t lastRecv = 0;
    uint16_t seq = 0;

    while (running_.load()) {
        uint32_t now = nowMs();

        if (!inputUp_.load() &&
            (lastHello == 0 || ageMs(now, lastHello) >= kHelloIntervalMs)) {
            WsuHello hello{};
            hello.role = WSU_ROLE_HOST;
            std::strncpy(hello.name, "wsu-host", WSU_NAME_LEN - 1);
            int n = wsu_pack_hello(buf, sizeof(buf), seq++, &hello);
            Endpoint target{consoleBase_.addr, consoleInputPort_};
            if (n > 0) inputSocket_.sendTo(target, buf, n);
            lastHello = now;
        }

        if (inputUp_.load() && ageMs(now, lastPing) >= kPingIntervalMs) {
            int n = wsu_pack_ping(buf, sizeof(buf), seq++, now);
            Endpoint ep;
            {
                std::lock_guard<std::mutex> lock(inputEpMutex_);
                ep = inputEp_;
            }
            if (n > 0) inputSocket_.sendTo(ep, buf, n);
            lastPing = now;
        }

        if (inputUp_.load() && lastRecv != 0 &&
            ageMs(now, lastRecv) > WSU_PEER_TIMEOUT_MS) {
            logWarn(kTag, "input link lost (timeout)");
            inputUp_.store(false);
            rttMs_.store(-1);
        }

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
        now = nowMs();
        lastRecv = now;

        switch (hdr.type) {
        case WSU_PKT_HELLO_ACK: {
            WsuHelloAck ack;
            if (wsu_parse_hello_ack(p, plen, &ack) < 0) break;
            if (ack.status == WSU_HELLO_OK) {
                {
                    std::lock_guard<std::mutex> lock(inputEpMutex_);
                    inputEp_ = from;
                }
                if (!inputUp_.exchange(true)) {
                    logInfo(kTag, "input link up: console %s",
                            from.toString().c_str());
                }
            } else {
                logError(kTag, "input HELLO rejected (status %u)",
                         ack.status);
            }
            break;
        }
        case WSU_PKT_PONG: {
            uint32_t echoed;
            if (wsu_parse_timestamp(p, plen, &echoed) == 4) {
                int sample = static_cast<int>(ageMs(now, echoed));
                int prev = rttMs_.load();
                rttMs_.store(prev < 0 ? sample : (prev * 7 + sample) / 8);
            }
            break;
        }
        default:
            break;
        }
    }
}

void WiiULink::streamLoop() {
    uint8_t buf[WSU_MAX_PACKET];
    uint32_t lastHello = 0;
    uint32_t lastPing = 0;
    uint32_t lastRecv = 0;
    Endpoint streamEp{consoleBase_.addr, consoleStreamPort_};
    uint16_t seq = 0;

    while (running_.load()) {
        uint32_t now = nowMs();

        if (!streamUp_.load() &&
            (lastHello == 0 || ageMs(now, lastHello) >= kHelloIntervalMs)) {
            WsuHello hello{};
            hello.role = WSU_ROLE_HOST;
            std::strncpy(hello.name, "wsu-host", WSU_NAME_LEN - 1);
            int n = wsu_pack_hello(buf, sizeof(buf), seq++, &hello);
            Endpoint target{consoleBase_.addr, consoleStreamPort_};
            if (n > 0) streamSocket_.sendTo(target, buf, n);
            lastHello = now;
        }

        // Keepalive: the console pauses the stream if the host goes quiet,
        // so ping the stream plugin as long as the link is up.
        if (streamUp_.load() && ageMs(now, lastPing) >= kPingIntervalMs) {
            int n = wsu_pack_ping(buf, sizeof(buf), seq++, now);
            if (n > 0) streamSocket_.sendTo(streamEp, buf, n);
            lastPing = now;
        }

        if (streamUp_.load() && lastRecv != 0 &&
            ageMs(now, lastRecv) > WSU_PEER_TIMEOUT_MS) {
            logWarn(kTag, "stream link lost (timeout)");
            streamUp_.store(false);
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
        lastRecv = nowMs();

        switch (hdr.type) {
        case WSU_PKT_HELLO_ACK: {
            WsuHelloAck ack;
            if (wsu_parse_hello_ack(p, plen, &ack) < 0) break;
            if (ack.status == WSU_HELLO_OK) {
                streamEp = from; // resolved (matters for broadcast HELLO)
                if (!streamUp_.exchange(true)) {
                    logInfo(kTag, "stream link up: console %s",
                            from.toString().c_str());
                }
            }
            break;
        }
        case WSU_PKT_CONFIG: {
            WsuConfig config;
            if (wsu_parse_config(p, plen, &config) < 0) break;
            if (callbacks_.onConfig) callbacks_.onConfig(config);
            break;
        }
        case WSU_PKT_VIDEO: {
            WsuVideoHeader v;
            const uint8_t *payload = nullptr;
            if (wsu_parse_video(p, plen, &v, &payload) < 0) break;
            if (callbacks_.onVideo) callbacks_.onVideo(buf, len, v, payload);
            break;
        }
        case WSU_PKT_AUDIO: {
            WsuAudioHeader a;
            const uint8_t *payload = nullptr;
            if (wsu_parse_audio(p, plen, &a, &payload) < 0) break;
            if (callbacks_.onAudio) callbacks_.onAudio(buf, len, a, payload);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace wsu
