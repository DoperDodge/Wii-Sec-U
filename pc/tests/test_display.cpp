// SdlDisplaySink test, run headless via SDL's dummy video/audio drivers
// (set through the test's CTest environment). Exercises the real code
// path: config → frames on a "receive" thread → pump on the main thread
// → texture upload, plus audio queueing and clean teardown.
// Only built when WSU_WITH_SDL is ON.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <thread>
#include <vector>

#include "test_common.h"
#include "util/time_util.h"
#include "video/sdl_display_sink.h"

using namespace wsu;

namespace {

AssembledFrame rawFrame(uint16_t w, uint16_t h, uint32_t id) {
    AssembledFrame f;
    f.frameId = id;
    f.timestampMs = id;
    f.flags = WSU_VIDEO_FLAG_KEYFRAME;
    f.data.assign(static_cast<size_t>(w) * h * 3,
                  static_cast<uint8_t>(id));
    return f;
}

} // namespace

int main() {
    auto sink = makeSdlDisplaySink("wsu display test");
    CHECK(sink != nullptr);
    if (sink == nullptr) return testSummary("test_display");

    WsuConfig config{};
    config.width = 96;
    config.height = 54;
    config.fps = 30;
    config.videoCodec = WSU_CODEC_RAWRGB;
    config.audioCodec = WSU_AUDIO_PCM16;
    config.audioRateHz = 48000;
    config.audioChannels = 2;
    sink->onConfig(config);

    // Frames arrive from a separate thread, as they do from the network
    // receive path in the host.
    std::thread feeder([&] {
        for (uint32_t id = 1; id <= 20; id++) {
            sink->onFrame(rawFrame(96, 54, id));
            std::vector<uint8_t> pcm(256 * 4, 0x11); // 256 stereo pairs BE
            sink->onAudio(pcm.data(), pcm.size(), id);
            sleepMs(5);
        }
    });

    // Pump like an app loop; the window must stay alive throughout.
    for (int i = 0; i < 30; i++) {
        CHECK(sink->pump());
        sleepMs(5);
    }
    feeder.join();
    CHECK(sink->pump()); // still alive after the feeder stops

    // Malformed frames must be ignored without breaking the display.
    {
        AssembledFrame bad;
        bad.frameId = 99;
        bad.data.assign(10, 0);
        sink->onFrame(bad);
        CHECK(sink->pump());
    }

    // Odd-sized audio payloads are ignored, not fatal.
    {
        std::vector<uint8_t> odd(7, 0);
        sink->onAudio(odd.data(), odd.size(), 0);
        CHECK(sink->pump());
    }

    sink.reset(); // clean teardown closes window/audio without crashing

    return testSummary("test_display");
}
