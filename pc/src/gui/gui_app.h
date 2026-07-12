// GuiApp — the graphical Wii-Sec-U app (Parsec-style launcher + viewer).
//
// One window, three screens:
//   Home    — enter a Wii U IP and "Start hosting", or a host address +
//             name and "Join" (last values remembered in wsu-app.ini)
//   Hosting — connection dashboard: console link state, player list,
//             stream stats, live preview of what remote players see
//   Joined  — the Wii U's screen full-window with a small HUD overlay
//
// The GUI thread owns SDL (window, events, gamepad polling) and renders
// with Dear ImGui on the SDL renderer. Sessions (HostApp/ClientApp) run
// on a background thread and exchange data through thread-safe pieces:
// GuiVideoView (frames/audio in) and SharedInput (controller out).
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "client/client_app.h"
#include "gui/gui_video_view.h"
#include "gui/shared_input.h"
#include "host/host_app.h"

struct SDL_Window;
struct SDL_Renderer;

namespace wsu {

class GuiApp {
  public:
    GuiApp() = default;
    ~GuiApp();

    // Runs the whole app; returns a process exit code.
    int run();

  private:
    enum class Mode { Idle, Hosting, Joined };

    bool init();
    void shutdown();
    void pollPad();
    void drawUi();
    void drawHome();
    void drawHosting();
    void drawJoined();
    void drawLogPanel();
    void drawVideoRegion(float reservedBottom);

    void startHosting();
    void startJoining();
    void stopSession();
    void loadSettings();
    void saveSettings();

    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    bool quit_ = false;

    Mode mode_ = Mode::Idle;
    std::shared_ptr<GuiVideoView> view_;
    SharedInput sharedInput_;
    std::unique_ptr<InputBackend> pad_; // polled on the GUI thread only
    std::unique_ptr<HostApp> host_;
    std::unique_ptr<ClientApp> client_;
    std::thread sessionThread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> sessionAlive_{false};

    // UI state.
    char consoleIp_[64] = "";
    char hostAddr_[64] = "";
    char playerName_[32] = "Player";
    std::string lastError_;
    bool showLogs_ = false;

    // 1 Hz HUD stats.
    uint64_t statFrames_ = 0;
    uint64_t statBytes_ = 0;
    uint32_t statStampMs_ = 0;
    unsigned fps_ = 0;
    double mbps_ = 0.0;
};

} // namespace wsu
