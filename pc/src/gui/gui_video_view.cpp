// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include "gui/gui_video_view.h"
#include "util/log.h"

namespace wsu {

namespace {
constexpr const char *kTag = "gui-video";
constexpr uint32_t kMaxQueuedAudioBytes = 48000 * 2 * 2 / 4; // ~250 ms
} // namespace

GuiVideoView::~GuiVideoView() { reset(); }

void GuiVideoView::onConfig(const WsuConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    hasConfig_.store(true);
}

void GuiVideoView::onFrame(const AssembledFrame &frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasConfig_.load()) return;
    DecodedFrame decoded;
    if (decoder_.decode(config_.videoCodec, config_.width, config_.height,
                        frame, decoded)) {
        frames_++;
        videoBytes_ += frame.data.size();
        pending_ = std::move(decoded);
        pendingFresh_ = true;
    }
}

void GuiVideoView::onAudio(const uint8_t *data, size_t len, uint32_t) {
    if (len < 4 || len % 4 != 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasConfig_.load() || config_.audioCodec != WSU_AUDIO_PCM16) {
            return;
        }
        if (audioDevice_ == 0 && !audioFailed_) {
            openAudio(config_.audioRateHz);
        }
    }
    if (audioDevice_ == 0) return;

    samples_.resize(len / 2);
    for (size_t i = 0; i < len / 2; i++) {
        samples_[i] =
            static_cast<int16_t>((data[i * 2] << 8) | data[i * 2 + 1]);
    }
    if (SDL_GetQueuedAudioSize(audioDevice_) <= kMaxQueuedAudioBytes) {
        SDL_QueueAudio(audioDevice_, samples_.data(),
                       static_cast<Uint32>(len));
    }
}

void GuiVideoView::openAudio(uint16_t rateHz) {
    SDL_AudioSpec want{};
    want.freq = rateHz > 0 ? rateHz : 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    SDL_AudioSpec have{};
    audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDevice_ == 0) {
        logWarn(kTag, "audio unavailable: %s", SDL_GetError());
        audioFailed_ = true;
        return;
    }
    SDL_PauseAudioDevice(audioDevice_, 0);
}

SDL_Texture *GuiVideoView::acquireTexture(SDL_Renderer *renderer) {
    DecodedFrame frame;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingFresh_) {
            frame = std::move(pending_);
            pendingFresh_ = false;
            fresh = true;
        }
    }

    if (fresh && frame.width > 0) {
        if (texture_ == nullptr || frame.width != texW_ ||
            frame.height != texH_) {
            if (texture_ != nullptr) SDL_DestroyTexture(texture_);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
            texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         frame.width, frame.height);
            texW_ = frame.width;
            texH_ = frame.height;
        }
        if (texture_ != nullptr) {
            SDL_UpdateTexture(texture_, nullptr, frame.rgb.data(),
                              frame.width * 3);
        }
    }
    return texture_;
}

void GuiVideoView::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    texW_ = 0;
    texH_ = 0;
    if (audioDevice_ != 0) {
        SDL_CloseAudioDevice(audioDevice_);
        audioDevice_ = 0;
    }
    audioFailed_ = false;
    pendingFresh_ = false;
    hasConfig_.store(false);
    frames_.store(0);
    videoBytes_.store(0);
}

} // namespace wsu
