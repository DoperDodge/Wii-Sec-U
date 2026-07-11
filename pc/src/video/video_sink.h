// Video/audio sinks. The stats sink is always available (headless relay,
// tests); an SDL display sink can be added behind WSU_WITH_SDL and an
// FFmpeg MJPEG decoder behind WSU_WITH_FFMPEG (PLAN.md §4B.2).
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "core/frame_assembler.h"

namespace wsu {

class VideoSink {
  public:
    virtual ~VideoSink() = default;

    // Configuration announced by the stream source (codec, resolution...).
    virtual void onConfig(const WsuConfig &config) = 0;

    // One complete reassembled frame. May be called from a receive thread.
    virtual void onFrame(const AssembledFrame &frame) = 0;

    // One audio packet (payload as described by the current config).
    virtual void onAudio(const uint8_t *data, size_t len,
                         uint32_t timestampMs) = 0;

    // Called regularly from the app's run loop (always the same thread).
    // Sinks with a UI use it to create/refresh their window; returning
    // false asks the app to shut down (user closed the window).
    virtual bool pump() { return true; }
};

// Counts frames/bytes; the CLI prints its numbers once per second.
class StatsSink final : public VideoSink {
  public:
    void onConfig(const WsuConfig &config) override {
        width_ = config.width;
        height_ = config.height;
        codec_ = config.videoCodec;
        haveConfig_ = true;
    }

    void onFrame(const AssembledFrame &frame) override {
        frames_++;
        videoBytes_ += frame.data.size();
        lastFrameId_ = frame.frameId;
    }

    void onAudio(const uint8_t *, size_t len, uint32_t) override {
        audioPackets_++;
        audioBytes_ += len;
    }

    uint64_t frames() const { return frames_.load(); }
    uint64_t videoBytes() const { return videoBytes_.load(); }
    uint64_t audioPackets() const { return audioPackets_.load(); }
    uint64_t audioBytes() const { return audioBytes_.load(); }
    uint32_t lastFrameId() const { return lastFrameId_.load(); }
    bool haveConfig() const { return haveConfig_.load(); }
    uint16_t width() const { return width_.load(); }
    uint16_t height() const { return height_.load(); }
    uint8_t codec() const { return codec_.load(); }

  private:
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> videoBytes_{0};
    std::atomic<uint64_t> audioPackets_{0};
    std::atomic<uint64_t> audioBytes_{0};
    std::atomic<uint32_t> lastFrameId_{0};
    std::atomic<bool> haveConfig_{false};
    std::atomic<uint16_t> width_{0};
    std::atomic<uint16_t> height_{0};
    std::atomic<uint8_t> codec_{0};
};

} // namespace wsu
