// GuiVideoView — VideoSink for the graphical app.
//
// Same decode-on-receive-thread, latest-wins design as SdlDisplaySink,
// but instead of owning a window it exposes the newest frame as an
// SDL_Texture for the GUI to draw wherever it wants (preview panel while
// hosting, full-window while joined). Audio playback is identical.
//
// Compiled only in SDL builds (the GUI target requires SDL anyway).
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "video/frame_decoder.h"
#include "video/video_sink.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace wsu {

class GuiVideoView final : public VideoSink {
  public:
    GuiVideoView() = default;
    ~GuiVideoView() override;

    // VideoSink (called from receive threads).
    void onConfig(const WsuConfig &config) override;
    void onFrame(const AssembledFrame &frame) override;
    void onAudio(const uint8_t *data, size_t len,
                 uint32_t timestampMs) override;

    // GUI thread: uploads the newest decoded frame (if any) and returns
    // the texture, or nullptr while nothing has arrived yet. The texture
    // belongs to this view; do not destroy it.
    SDL_Texture *acquireTexture(SDL_Renderer *renderer);

    int frameWidth() const { return texW_; }
    int frameHeight() const { return texH_; }

    // Rolling stats for the HUD.
    uint64_t framesReceived() const { return frames_.load(); }
    uint64_t videoBytes() const { return videoBytes_.load(); }
    bool hasConfig() const { return hasConfig_.load(); }

    // Drops the texture/audio device (call when a session ends).
    void reset();

  private:
    void openAudio(uint16_t rateHz);

    std::mutex mutex_;
    WsuConfig config_{};
    FrameDecoder decoder_;
    DecodedFrame pending_;
    bool pendingFresh_ = false;

    SDL_Texture *texture_ = nullptr;
    int texW_ = 0;
    int texH_ = 0;

    uint32_t audioDevice_ = 0;
    bool audioFailed_ = false;
    std::vector<int16_t> samples_;

    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> videoBytes_{0};
    std::atomic<bool> hasConfig_{false};
};

} // namespace wsu
