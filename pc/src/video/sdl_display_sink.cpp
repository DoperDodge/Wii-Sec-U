// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#if defined(WSU_WITH_SDL)

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <algorithm>
#include <mutex>

#include "util/log.h"
#include "util/time_util.h"
#include "video/frame_decoder.h"
#include "video/sdl_display_sink.h"

namespace wsu {

namespace {

constexpr const char *kTag = "display";
// Keep at most ~250 ms of audio queued; beyond that we're a broadcast
// delay line, not a game stream.
constexpr uint32_t kMaxQueuedAudioBytes = 48000 * 2 * 2 / 4;

class SdlDisplaySink final : public VideoSink {
  public:
    explicit SdlDisplaySink(std::string title) : title_(std::move(title)) {
        SDL_SetMainReady();
        SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    }

    ~SdlDisplaySink() override {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        if (audioDevice_ != 0) SDL_CloseAudioDevice(audioDevice_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    }

    void onConfig(const WsuConfig &config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        haveConfig_ = true;
    }

    void onFrame(const AssembledFrame &frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!haveConfig_) return;
        DecodedFrame decoded;
        if (decoder_.decode(config_.videoCodec, config_.width,
                            config_.height, frame, decoded)) {
            pending_ = std::move(decoded);
            pendingFresh_ = true;
        }
    }

    void onAudio(const uint8_t *data, size_t len, uint32_t) override {
        if (len < 4 || len % 4 != 0) return; // stereo int16 pairs
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!haveConfig_ || config_.audioCodec != WSU_AUDIO_PCM16) {
                return;
            }
            if (audioDevice_ == 0 && !audioFailed_) openAudio();
        }
        if (audioDevice_ == 0) return;

        // Wire is big-endian; convert to native for AUDIO_S16SYS.
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

    bool pump() override {
        if (closed_) return false;
        if (window_ == nullptr && !createWindow()) {
            closed_ = true;
            return false;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                logInfo(kTag, "window closed");
                closed_ = true;
                return false;
            }
        }

        bool fresh = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingFresh_) {
                current_ = std::move(pending_);
                pendingFresh_ = false;
                fresh = true;
            }
        }

        if (fresh && current_.width > 0) {
            ensureTexture(current_.width, current_.height);
            if (texture_ != nullptr) {
                SDL_UpdateTexture(texture_, nullptr, current_.rgb.data(),
                                  current_.width * 3);
                shownFrames_++;
            }
        }

        render();
        updateTitle();
        return true;
    }

  private:
    bool createWindow() {
        // Lazy creation keeps all window/renderer work on the pump thread.
        window_ = SDL_CreateWindow(
            title_.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            856, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (window_ == nullptr) {
            logError(kTag, "SDL_CreateWindow: %s", SDL_GetError());
            return false;
        }
        renderer_ = SDL_CreateRenderer(
            window_, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer_ == nullptr) {
            // Dummy/headless drivers have no accelerated renderer.
            renderer_ = SDL_CreateRenderer(window_, -1, 0);
        }
        if (renderer_ == nullptr) {
            logError(kTag, "SDL_CreateRenderer: %s", SDL_GetError());
            return false;
        }
        logInfo(kTag, "video driver: %s", SDL_GetCurrentVideoDriver());
        return true;
    }

    void ensureTexture(int w, int h) {
        if (texture_ != nullptr && w == texW_ && h == texH_) return;
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
        if (texture_ == nullptr) {
            logError(kTag, "SDL_CreateTexture: %s", SDL_GetError());
            return;
        }
        texW_ = w;
        texH_ = h;
    }

    void render() {
        if (renderer_ == nullptr) return;
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (texture_ != nullptr) {
            // Letterbox: preserve the stream's aspect inside the window.
            int winW = 0, winH = 0;
            SDL_GetRendererOutputSize(renderer_, &winW, &winH);
            SDL_Rect dst{0, 0, winW, winH};
            if (winW > 0 && winH > 0 && texW_ > 0 && texH_ > 0) {
                float scale = std::min(static_cast<float>(winW) / texW_,
                                       static_cast<float>(winH) / texH_);
                dst.w = static_cast<int>(texW_ * scale);
                dst.h = static_cast<int>(texH_ * scale);
                dst.x = (winW - dst.w) / 2;
                dst.y = (winH - dst.h) / 2;
            }
            SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
        }
        SDL_RenderPresent(renderer_);
    }

    void updateTitle() {
        uint32_t now = nowMs();
        if (ageMs(now, lastTitleMs_) < 1000) return;
        char title[128];
        SDL_snprintf(title, sizeof(title), "%s — %dx%d %ufps",
                     title_.c_str(), texW_, texH_,
                     static_cast<unsigned>(shownFrames_ - lastShown_));
        SDL_SetWindowTitle(window_, title);
        lastShown_ = shownFrames_;
        lastTitleMs_ = now;
    }

    void openAudio() {
        SDL_AudioSpec want{};
        want.freq = config_.audioRateHz > 0 ? config_.audioRateHz : 48000;
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
        logInfo(kTag, "audio out: %d Hz, %u ch", have.freq, have.channels);
    }

    std::string title_;
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    int texW_ = 0;
    int texH_ = 0;
    bool closed_ = false;

    std::mutex mutex_;
    WsuConfig config_{};
    bool haveConfig_ = false;
    FrameDecoder decoder_;
    DecodedFrame pending_;
    bool pendingFresh_ = false;
    DecodedFrame current_;

    SDL_AudioDeviceID audioDevice_ = 0;
    bool audioFailed_ = false;
    std::vector<int16_t> samples_;

    uint64_t shownFrames_ = 0;
    uint64_t lastShown_ = 0;
    uint32_t lastTitleMs_ = 0;
};

} // namespace

std::unique_ptr<VideoSink> makeSdlDisplaySink(const std::string &title) {
    return std::make_unique<SdlDisplaySink>(title);
}

} // namespace wsu

#else // !WSU_WITH_SDL

#include "video/sdl_display_sink.h"

namespace wsu {
std::unique_ptr<VideoSink> makeSdlDisplaySink(const std::string &) {
    return nullptr;
}
} // namespace wsu

#endif // WSU_WITH_SDL
