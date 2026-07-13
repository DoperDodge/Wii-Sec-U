// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "gui/gui_app.h"
#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {

constexpr const char *kTag = "gui";
constexpr const char *kSettingsFile = "wsu-app.ini";

// ---- log capture for the in-app log panel ----
std::mutex gLogMutex;
std::deque<std::string> gLogLines;

void guiLogSink(LogLevel, const char *line) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLogLines.emplace_back(line);
    while (gLogLines.size() > 200) gLogLines.pop_front();
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.FramePadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(10, 10);

    ImVec4 accent(0.51f, 0.36f, 0.96f, 1.00f); // Parsec-ish purple
    ImVec4 accentHover(0.60f, 0.47f, 1.00f, 1.00f);
    ImVec4 accentActive(0.42f, 0.28f, 0.85f, 1.00f);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.075f, 0.09f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_Button] = accent;
    c[ImGuiCol_ButtonHovered] = accentHover;
    c[ImGuiCol_ButtonActive] = accentActive;
    c[ImGuiCol_Header] = accent;
    c[ImGuiCol_HeaderHovered] = accentHover;
    c[ImGuiCol_HeaderActive] = accentActive;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentActive;
}

void statusDot(bool up, const char *label) {
    ImVec4 color = up ? ImVec4(0.30f, 0.85f, 0.39f, 1.0f)
                      : ImVec4(0.85f, 0.30f, 0.30f, 1.0f);
    ImGui::TextColored(color, "\xe2\x97\x8f"); // ●
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

} // namespace

GuiApp::~GuiApp() { shutdown(); }

bool GuiApp::init() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                 SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    window_ = SDL_CreateWindow("Wii-Sec-U", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, 1100, 660,
                               SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_ALLOW_HIGHDPI);
    if (window_ == nullptr) return false;
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        renderer_ = SDL_CreateRenderer(window_, -1, 0);
    }
    if (renderer_ == nullptr) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no layout file clutter
    applyTheme();
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);

    setLogSink(&guiLogSink);
    view_ = std::make_shared<GuiVideoView>();
    pad_ = makeSdlBackend();
    loadSettings();
    return true;
}

void GuiApp::shutdown() {
    stopSession();
    setLogSink(nullptr);
    view_.reset();
    pad_.reset();
    if (renderer_ != nullptr || window_ != nullptr) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

int GuiApp::run() {
    if (!init()) return 1;

    while (!quit_) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) quit_ = true;
        }

        pollPad();

        // A session thread that ended on its own (bind failure, fatal
        // socket error) drops us back to Home with the log visible.
        if (mode_ != Mode::Idle && !sessionAlive_.load() &&
            sessionThread_.joinable()) {
            stopSession();
            lastError_ = "Session ended — see logs for details.";
            showLogs_ = true;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawUi();
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer_, 13, 13, 16, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),
                                              renderer_);
        SDL_RenderPresent(renderer_);
    }

    shutdown();
    return 0;
}

void GuiApp::pollPad() {
    if (mode_ == Mode::Idle || pad_ == nullptr) return;
    WsuInputState state;
    if (pad_->poll(state)) {
        sharedInput_.set(state);
    }
}

// ------------------------------------------------------------------
// Screens
// ------------------------------------------------------------------

void GuiApp::drawUi() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    switch (mode_) {
    case Mode::Idle: drawHome(); break;
    case Mode::Hosting: drawHosting(); break;
    case Mode::Joined: drawJoined(); break;
    }
    ImGui::End();
}

void GuiApp::drawHome() {
    const float cardWidth = 460.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Top margin as a real item (Dummy), and horizontal centering by
    // indenting to the card's left edge — neither extends the parent
    // boundary the way a trailing SetCursorPos would.
    ImGui::Dummy(ImVec2(1.0f, 30.0f));
    float indent = (avail.x - cardWidth) * 0.5f;
    if (indent > 0.0f) ImGui::SetCursorPosX(indent);

    ImGui::BeginChild("card", ImVec2(cardWidth, 0),
                      ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextUnformatted("Wii-Sec-U");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Wii U remote co-op — play together from anywhere");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("HOST  (this PC is on the Wii U's network)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##console", "Wii U IP — leave blank to "
                                          "auto-discover on the LAN",
                             consoleIp_, sizeof(consoleIp_));
    if (ImGui::Button("Start hosting", ImVec2(-FLT_MIN, 42))) {
        startHosting();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("JOIN  (a friend is hosting)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##host", "Host address, e.g. 203.0.113.7",
                             hostAddr_, sizeof(hostAddr_));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Your player name", playerName_,
                             sizeof(playerName_));
    if (ImGui::Button("Join", ImVec2(-FLT_MIN, 42))) {
        startJoining();
    }

    ImGui::Spacing();
    if (!lastError_.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           lastError_.c_str());
    }
    bool padOk = pad_ != nullptr;
    ImGui::TextDisabled(padOk ? "Controller: SDL (plug in any pad)"
                              : "Controller: none detected");
    ImGui::Checkbox("Show logs", &showLogs_);
    ImGui::EndChild();

    if (showLogs_) drawLogPanel();
}

void GuiApp::drawVideoRegion(float reservedBottom) {
    SDL_Texture *texture = view_->acquireTexture(renderer_);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= reservedBottom;
    if (avail.x < 32 || avail.y < 32) return;

    // A child window contains the centered content so cursor positioning
    // stays inside its own boundaries — the child is submitted as a single
    // item to the parent, which keeps ImGui's boundary tracking happy.
    ImGui::BeginChild("##video", avail, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImVec2 region = ImGui::GetContentRegionAvail();

    if (texture == nullptr) {
        const char *msg = "Waiting for video...";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((region.x - textSize.x) * 0.5f,
                                   (region.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
    } else {
        float texW = static_cast<float>(view_->frameWidth());
        float texH = static_cast<float>(view_->frameHeight());
        float scale =
            texW > 0 && texH > 0
                ? (region.x / texW < region.y / texH ? region.x / texW
                                                      : region.y / texH)
                : 1.0f;
        ImVec2 size(texW * scale, texH * scale);
        ImGui::SetCursorPos(ImVec2((region.x - size.x) * 0.5f,
                                   (region.y - size.y) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(
                         reinterpret_cast<uintptr_t>(texture)),
                     size);
    }
    ImGui::EndChild();
}

void GuiApp::drawHosting() {
    // Header row.
    ImGui::TextUnformatted("Hosting");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
    if (ImGui::Button("Stop", ImVec2(90, 0))) {
        stopSession();
        return;
    }
    ImGui::Separator();

    // Refresh 1 Hz stats.
    uint32_t now = nowMs();
    if (ageMs(now, statStampMs_) >= 1000) {
        uint64_t frames = view_->framesReceived();
        uint64_t bytes = view_->videoBytes();
        fps_ = static_cast<unsigned>(frames - statFrames_);
        mbps_ = static_cast<double>(bytes - statBytes_) * 8.0 / 1e6;
        statFrames_ = frames;
        statBytes_ = bytes;
        statStampMs_ = now;
    }

    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 320);

    // Left column: link status + players.
    statusDot(host_->link().inputLinkUp(), "Console input link");
    statusDot(host_->link().streamLinkUp(), "Console video link");
    ImGui::Text("Console RTT: %d ms", host_->link().rttMs());
    ImGui::Text("Stream: %u fps, %.2f Mbit/s", fps_, mbps_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Players");
    ImGui::Separator();
    ImGui::TextUnformatted("P1  you (this PC)");
    auto clients = host_->sessions().clients();
    for (const auto &c : clients) {
        ImGui::Text("P%u  %s  (%s, %d ms)", c.slot + 1, c.name.c_str(),
                    c.endpoint.c_str(), c.rttMs);
    }
    for (size_t i = clients.size(); i < 3; i++) {
        ImGui::TextDisabled("P%zu  waiting for player...", i + 2);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Friends join with your public IP\n(forward UDP "
                        "4405 to this PC)");
    ImGui::Checkbox("Show logs", &showLogs_);

    // Right column: live preview.
    ImGui::NextColumn();
    ImGui::TextDisabled("Preview (what remote players see)");
    drawVideoRegion(0.0f);
    ImGui::Columns(1);

    if (showLogs_) drawLogPanel();
}

void GuiApp::drawJoined() {
    // HUD line, then the video filling the rest.
    bool connected = client_->connected();
    if (connected) {
        uint32_t now = nowMs();
        if (ageMs(now, statStampMs_) >= 1000) {
            uint64_t frames = view_->framesReceived();
            fps_ = static_cast<unsigned>(frames - statFrames_);
            statFrames_ = frames;
            statStampMs_ = now;
        }
        ImGui::Text("P%u  |  %u fps  |  %d ms", client_->slot() + 1, fps_,
                    client_->rttMs());
    } else {
        ImGui::TextUnformatted("Connecting to host...");
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110);
    if (ImGui::Button("Disconnect", ImVec2(110, 0))) {
        stopSession();
        return;
    }
    drawVideoRegion(0.0f);
}

void GuiApp::drawLogPanel() {
    ImGui::SetNextWindowSize(ImVec2(640, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Logs", &showLogs_)) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        for (const auto &line : gLogLines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------------
// Sessions
// ------------------------------------------------------------------

void GuiApp::startHosting() {
    lastError_.clear();
    HostOptions opts;
    if (consoleIp_[0] != '\0') {
        opts.consoleAddr = Endpoint::parse(consoleIp_, 1);
        if (!opts.consoleAddr.valid()) {
            lastError_ = "That Wii U IP doesn't look valid.";
            return;
        }
    } else {
        opts.consoleAddr = Endpoint::broadcast(1);
    }
    opts.printStats = false;

    saveSettings();
    view_->reset();
    host_ = std::make_unique<HostApp>(
        opts, makeSharedInputBackend(sharedInput_), view_);
    stopFlag_.store(false);
    sessionAlive_.store(true);
    sessionThread_ = std::thread([this] {
        host_->run(stopFlag_);
        sessionAlive_.store(false);
    });
    mode_ = Mode::Hosting;
    logInfo(kTag, "hosting started");
}

void GuiApp::startJoining() {
    lastError_.clear();
    ClientOptions opts;
    opts.hostAddr = Endpoint::parse(hostAddr_, WSU_HOST_PORT);
    if (!opts.hostAddr.valid()) {
        lastError_ = "Enter the host's address first (e.g. 203.0.113.7).";
        return;
    }
    opts.playerName = playerName_[0] != '\0' ? playerName_ : "player";
    opts.printStats = false;

    saveSettings();
    view_->reset();
    client_ = std::make_unique<ClientApp>(
        opts, makeSharedInputBackend(sharedInput_), view_);
    stopFlag_.store(false);
    sessionAlive_.store(true);
    sessionThread_ = std::thread([this] {
        client_->run(stopFlag_);
        sessionAlive_.store(false);
    });
    mode_ = Mode::Joined;
    logInfo(kTag, "joining %s", hostAddr_);
}

void GuiApp::stopSession() {
    stopFlag_.store(true);
    if (sessionThread_.joinable()) sessionThread_.join();
    host_.reset();
    client_.reset();
    sharedInput_.clear();
    if (view_) view_->reset();
    sessionAlive_.store(false);
    mode_ = Mode::Idle;
    statFrames_ = 0;
    statBytes_ = 0;
    fps_ = 0;
    mbps_ = 0.0;
}

// ------------------------------------------------------------------
// Settings persistence (tiny key=value file next to the working dir)
// ------------------------------------------------------------------

void GuiApp::loadSettings() {
    FILE *f = std::fopen(kSettingsFile, "r");
    if (f == nullptr) return;
    char line[160];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char *eq = std::strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char *value = eq + 1;
        value[std::strcspn(value, "\r\n")] = '\0';
        if (std::strcmp(line, "console") == 0) {
            std::snprintf(consoleIp_, sizeof(consoleIp_), "%s", value);
        } else if (std::strcmp(line, "host") == 0) {
            std::snprintf(hostAddr_, sizeof(hostAddr_), "%s", value);
        } else if (std::strcmp(line, "name") == 0) {
            std::snprintf(playerName_, sizeof(playerName_), "%s", value);
        }
    }
    std::fclose(f);
}

void GuiApp::saveSettings() {
    FILE *f = std::fopen(kSettingsFile, "w");
    if (f == nullptr) return;
    std::fprintf(f, "console=%s\nhost=%s\nname=%s\n", consoleIp_,
                 hostAddr_, playerName_);
    std::fclose(f);
}

} // namespace wsu
