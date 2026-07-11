// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "client/client_app.h"

#include <cstring>

#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {
constexpr const char *kTag = "client";
constexpr uint32_t kHelloIntervalMs = 1000;
constexpr uint32_t kPingIntervalMs = 1000;
} // namespace

ClientApp::ClientApp(ClientOptions options,
                     std::unique_ptr<InputBackend> input,
                     std::shared_ptr<VideoSink> sink)
    : options_(options), input_(std::move(input)), sink_(std::move(sink)) {}

bool ClientApp::run(const std::atomic<bool> &stop) {
    UdpSocket socket;
    if (!socket.open(0)) {
        logError(kTag, "failed to open socket");
        return false;
    }

    const uint32_t tickMs =
        options_.inputRateHz > 0 ? 1000 / options_.inputRateHz : 8;
    socket.setRecvTimeout(tickMs);

    FrameAssembler assembler([this](AssembledFrame &&frame) {
        if (sink_) sink_->onFrame(frame);
    });

    logInfo(kTag, "connecting to host %s as '%s'",
            options_.hostAddr.toString().c_str(),
            options_.playerName.c_str());

    uint8_t buf[WSU_MAX_PACKET];
    uint16_t seq = 0;
    uint32_t lastHello = 0;
    uint32_t lastPing = 0;
    uint32_t lastInput = 0;
    uint32_t lastRecv = 0;
    uint32_t lastStats = nowMs();
    uint64_t lastFrames = 0;
    uint64_t lastBytes = 0;

    while (!stop.load()) {
        uint32_t now = nowMs();

        if (!connected_.load() &&
            (lastHello == 0 || ageMs(now, lastHello) >= kHelloIntervalMs)) {
            WsuHello hello{};
            hello.role = WSU_ROLE_CLIENT;
            std::strncpy(hello.name, options_.playerName.c_str(),
                         WSU_NAME_LEN - 1);
            int n = wsu_pack_hello(buf, sizeof(buf), seq++, &hello);
            if (n > 0) socket.sendTo(options_.hostAddr, buf, n);
            lastHello = now;
        }

        if (connected_.load()) {
            if (lastRecv != 0 &&
                ageMs(now, lastRecv) > WSU_PEER_TIMEOUT_MS) {
                logWarn(kTag, "host lost (timeout), reconnecting");
                connected_.store(false);
                slot_.store(WSU_NO_SLOT);
                rttMs_.store(-1);
            }

            if (input_ && ageMs(now, lastInput) >= tickMs) {
                WsuInputState state;
                if (input_->poll(state)) {
                    state.slot = slot_.load(); // informational; host
                                               // enforces the real slot
                    int n = wsu_pack_input(buf, sizeof(buf), seq++, now,
                                           &state);
                    if (n > 0) socket.sendTo(options_.hostAddr, buf, n);
                }
                lastInput = now;
            }

            if (ageMs(now, lastPing) >= kPingIntervalMs) {
                int n = wsu_pack_ping(buf, sizeof(buf), seq++, now);
                if (n > 0) socket.sendTo(options_.hostAddr, buf, n);
                lastPing = now;
            }
        }

        size_t len = 0;
        Endpoint from;
        RecvResult r = socket.recvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::Error) {
            if (!stop.load()) sleepMs(50);
            continue;
        }
        if (r == RecvResult::Ok && from == options_.hostAddr) {
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
                    slot_.store(ack.slot);
                    if (!connected_.exchange(true)) {
                        logInfo(kTag, "connected as player %u",
                                ack.slot + 1);
                    }
                } else {
                    logError(kTag,
                             "join rejected (status %u: %s)", ack.status,
                             ack.status == WSU_HELLO_FULL
                                 ? "session full"
                                 : ack.status ==
                                           WSU_HELLO_VERSION_MISMATCH
                                       ? "version mismatch"
                                       : "rejected");
                    connected_.store(false);
                }
                break;
            }
            case WSU_PKT_CONFIG: {
                WsuConfig config;
                if (wsu_parse_config(p, plen, &config) >= 0 && sink_) {
                    sink_->onConfig(config);
                }
                break;
            }
            case WSU_PKT_VIDEO: {
                WsuVideoHeader v;
                const uint8_t *payload = nullptr;
                if (wsu_parse_video(p, plen, &v, &payload) >= 0) {
                    assembler.feed(v, payload);
                }
                break;
            }
            case WSU_PKT_AUDIO: {
                WsuAudioHeader a;
                const uint8_t *payload = nullptr;
                if (wsu_parse_audio(p, plen, &a, &payload) >= 0 && sink_) {
                    sink_->onAudio(payload, a.payloadLen, a.timestampMs);
                }
                break;
            }
            case WSU_PKT_PONG: {
                uint32_t echoed;
                if (wsu_parse_timestamp(p, plen, &echoed) == 4) {
                    int sample = static_cast<int>(ageMs(now, echoed));
                    int prev = rttMs_.load();
                    rttMs_.store(prev < 0 ? sample
                                          : (prev * 7 + sample) / 8);
                }
                break;
            }
            case WSU_PKT_BYE:
                logWarn(kTag, "host said goodbye");
                connected_.store(false);
                slot_.store(WSU_NO_SLOT);
                break;
            default:
                break;
            }
        }

        // Drive the display (window events, latest-frame upload). A false
        // return means the user closed the window.
        if (sink_ && !sink_->pump()) {
            logInfo(kTag, "display closed, exiting");
            break;
        }

        if (options_.printStats && ageMs(nowMs(), lastStats) >= 1000) {
            const auto *stats = dynamic_cast<const StatsSink *>(sink_.get());
            if (stats != nullptr && connected_.load()) {
                uint64_t frames = stats->frames();
                uint64_t bytes = stats->videoBytes();
                logInfo(kTag, "video %ufps %.2fMbps rtt=%dms (P%u)",
                        static_cast<unsigned>(frames - lastFrames),
                        static_cast<double>(bytes - lastBytes) * 8.0 / 1e6,
                        rttMs_.load(), slot_.load() + 1);
                lastFrames = frames;
                lastBytes = bytes;
            }
            lastStats = nowMs();
        }
    }

    if (connected_.load()) {
        int n = wsu_pack_bye(buf, sizeof(buf), seq++, WSU_BYE_QUIT);
        if (n > 0) socket.sendTo(options_.hostAddr, buf, n);
    }
    return true;
}

} // namespace wsu
