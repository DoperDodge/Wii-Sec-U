// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "host/host_app.h"

#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {
constexpr const char *kTag = "host";
} // namespace

HostApp::HostApp(HostOptions options,
                 std::unique_ptr<InputBackend> localInput,
                 std::shared_ptr<VideoSink> localSink)
    : options_(options), localInput_(std::move(localInput)),
      localSink_(std::move(localSink)), server_(hub_, options_.listenPort),
      link_(options_.consoleAddr, options_.consoleInputPort,
            options_.consoleStreamPort,
            WiiULink::Callbacks{
                [this](const uint8_t *d, size_t l, const WsuVideoHeader &v,
                       const uint8_t *p) { onVideo(d, l, v, p); },
                [this](const uint8_t *d, size_t l, const WsuAudioHeader &a,
                       const uint8_t *p) { onAudio(d, l, a, p); },
                [this](const WsuConfig &c) { onConfig(c); }}),
      assembler_([this](AssembledFrame &&frame) {
          if (localSink_) localSink_->onFrame(frame);
      }) {}

HostApp::~HostApp() = default;

const StatsSink *HostApp::statsSink() const {
    return dynamic_cast<const StatsSink *>(localSink_.get());
}

void HostApp::onVideo(const uint8_t *datagram, size_t len,
                      const WsuVideoHeader &v, const uint8_t *payload) {
    server_.broadcast(datagram, len);
    assembler_.feed(v, payload);
}

void HostApp::onAudio(const uint8_t *datagram, size_t len,
                      const WsuAudioHeader &a, const uint8_t *payload) {
    server_.broadcast(datagram, len);
    if (localSink_) localSink_->onAudio(payload, a.payloadLen, a.timestampMs);
}

void HostApp::onConfig(const WsuConfig &config) {
    server_.setConfig(config);
    if (localSink_) localSink_->onConfig(config);
}

bool HostApp::run(const std::atomic<bool> &stop) {
    if (!server_.start()) return false;
    if (!link_.start()) {
        server_.stop();
        return false;
    }

    logInfo(kTag, "host running: console %s, clients on UDP %u",
            options_.consoleAddr.toString().c_str(), server_.port());

    const uint32_t tickMs =
        options_.inputRateHz > 0 ? 1000 / options_.inputRateHz : 8;
    uint8_t bundle[WSU_MAX_PACKET];
    uint16_t seq = 0;
    uint32_t lastStats = nowMs();
    uint64_t lastFrames = 0;
    uint64_t lastBytes = 0;

    while (!stop.load()) {
        uint32_t now = nowMs();

        // P1 = local controller.
        if (localInput_) {
            WsuInputState state;
            if (localInput_->poll(state)) {
                hub_.set(0, state, now);
            }
        }

        int n = hub_.packBundle(bundle, sizeof(bundle), seq++, now);
        if (n > 0) link_.sendInputBundle(bundle, n);

        if (options_.printStats && ageMs(now, lastStats) >= 1000) {
            const StatsSink *stats = statsSink();
            if (stats != nullptr) {
                uint64_t frames = stats->frames();
                uint64_t bytes = stats->videoBytes();
                logInfo(kTag,
                        "video %ufps %.2fMbps | input=%s stream=%s rtt=%dms "
                        "| clients=%zu",
                        static_cast<unsigned>(frames - lastFrames),
                        static_cast<double>(bytes - lastBytes) * 8.0 / 1e6,
                        link_.inputLinkUp() ? "up" : "down",
                        link_.streamLinkUp() ? "up" : "down", link_.rttMs(),
                        server_.clients().size());
                lastFrames = frames;
                lastBytes = bytes;
            }
            lastStats = now;
        }

        sleepMs(tickMs);
    }

    link_.stop();
    server_.stop();
    return true;
}

} // namespace wsu
