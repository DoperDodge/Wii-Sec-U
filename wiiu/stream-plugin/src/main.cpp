// wsu-stream — Aroma (WUPS) plugin that captures the TV picture and audio
// and streams them to the host PC (PLAN.md §4A.1–4A.4).
//
//   GX2 scan-out hook ─► capture surfaces ─► encoder thread (JPEG)
//                                                  │
//   AX final-mix tap ─► PCM ring buffer ─► net thread ─► host PC (UDP :4406)
//
// Everything idles (no captures, no encoding, no sends) until a host
// completes the HELLO handshake, so the plugin costs almost nothing when
// nobody is streaming.
//
// Settings live in the Aroma plugin config menu (open it in-game with
// L + D-Pad Down + SELECT): resolution preset, frame rate, JPEG quality
// and the audio toggle apply immediately; the port applies on the next
// game launch. Values persist via the WUPS storage API.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <cstdio>

#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>
#include <wups/storage.h>

#include "stream_state.h"
#include "wsu_protocol.h"
#include "wsu_wiiu.h"

WUPS_PLUGIN_NAME("wsu-stream");
WUPS_PLUGIN_DESCRIPTION("Wii-Sec-U: TV video/audio streaming to the host PC");
WUPS_PLUGIN_VERSION("v0.2.0");
WUPS_PLUGIN_AUTHOR("Wii-Sec-U contributors");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("wsu_stream");

namespace wsu {

StreamConfig gConfig;

namespace {

// Resolution presets (index stored in the WUPS storage).
struct Preset {
    int width;
    int height;
    const char *label;
};
constexpr Preset kPresets[] = {
    {214, 120, "214x120 (fastest)"},
    {428, 240, "428x240 (default)"},
    {640, 360, "640x360"},
    {854, 480, "854x480 (slowest)"},
};
constexpr int kDefaultPreset = 1;
constexpr int kDefaultFps = 20;
constexpr int kDefaultQuality = 60;
constexpr bool kDefaultAudio = true;

int32_t gPresetIndex = kDefaultPreset;

void applyPreset(int index) {
    if (index < 0 || index >= static_cast<int>(sizeof(kPresets) /
                                               sizeof(kPresets[0]))) {
        index = kDefaultPreset;
    }
    gPresetIndex = index;
    gConfig.width = kPresets[index].width;
    gConfig.height = kPresets[index].height;
}

// ---- config menu callbacks ----

void presetChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    applyPreset(static_cast<int>(newValue));
    WUPSStorageAPI::Store("preset", gPresetIndex);
}

void fpsChanged(ConfigItemIntegerRange *, int32_t newValue) {
    gConfig.fps = newValue;
    WUPSStorageAPI::Store("fps", static_cast<int32_t>(gConfig.fps));
}

void qualityChanged(ConfigItemIntegerRange *, int32_t newValue) {
    gConfig.quality = newValue;
    WUPSStorageAPI::Store("quality", static_cast<int32_t>(gConfig.quality));
}

void audioChanged(ConfigItemBoolean *, bool newValue) {
    gConfig.audio = newValue;
    WUPSStorageAPI::Store("audio", gConfig.audio);
}

WUPSConfigAPICallbackStatus configMenuOpened(
    WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);

        static char status[64];
        std::snprintf(status, sizeof(status), "Host PC: %s",
                      netHostActive() ? "connected, streaming"
                                      : "not connected");
        root.add(WUPSConfigItemStub::Create(status));

        constexpr WUPSConfigItemMultipleValues::ValuePair presetValues[] = {
            {0, kPresets[0].label},
            {1, kPresets[1].label},
            {2, kPresets[2].label},
            {3, kPresets[3].label},
        };
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
            "preset", "Stream resolution", kDefaultPreset, gPresetIndex,
            presetValues, presetChanged));

        root.add(WUPSConfigItemIntegerRange::Create(
            "fps", "Frame rate cap (higher = more game slowdown)",
            kDefaultFps, gConfig.fps, 5, 30, fpsChanged));

        root.add(WUPSConfigItemIntegerRange::Create(
            "quality", "JPEG quality", kDefaultQuality, gConfig.quality, 10,
            95, qualityChanged));

        root.add(WUPSConfigItemBoolean::Create(
            "audio", "Stream TV audio", kDefaultAudio, gConfig.audio,
            audioChanged));

        static char portText[48];
        std::snprintf(portText, sizeof(portText),
                      "UDP port: %d (fixed)", gConfig.port);
        root.add(WUPSConfigItemStub::Create(portText));
    } catch (std::exception &e) {
        WSU_LOG("stream: config menu failed: %s", e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void configMenuClosed() {
    WUPSStorageAPI::SaveStorage();
}

void loadSettings() {
    int32_t preset = kDefaultPreset;
    int32_t fps = kDefaultFps;
    int32_t quality = kDefaultQuality;
    bool audio = kDefaultAudio;
    WUPSStorageAPI::GetOrStoreDefault("preset", preset,
                                      static_cast<int32_t>(kDefaultPreset));
    WUPSStorageAPI::GetOrStoreDefault("fps", fps,
                                      static_cast<int32_t>(kDefaultFps));
    WUPSStorageAPI::GetOrStoreDefault(
        "quality", quality, static_cast<int32_t>(kDefaultQuality));
    WUPSStorageAPI::GetOrStoreDefault("audio", audio, kDefaultAudio);
    WUPSStorageAPI::SaveStorage();

    applyPreset(preset);
    gConfig.fps = fps < 5 ? 5 : (fps > 30 ? 30 : fps);
    gConfig.quality = quality < 10 ? 10 : (quality > 95 ? 95 : quality);
    gConfig.audio = audio;
    gConfig.port = WSU_CONSOLE_STREAM_PORT;

    WSU_LOG("wsu-stream config: %dx%d@%dfps q%d audio %d port %d",
            gConfig.width, gConfig.height, gConfig.fps, gConfig.quality,
            gConfig.audio ? 1 : 0, gConfig.port);
}

} // namespace
} // namespace wsu

INITIALIZE_PLUGIN() {
    wsu::logInit();

    WUPSConfigAPIOptionsV1 configOptions = {.name = "Wii-Sec-U stream"};
    if (WUPSConfigAPI_Init(configOptions, wsu::configMenuOpened,
                           wsu::configMenuClosed) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        WSU_LOG("stream: failed to init config API");
    }
    wsu::loadSettings();
}

ON_APPLICATION_START() {
    wsu::logInit();
    wsu::loadSettings();
    wsu::audioInit();
    wsu::captureInit();
    if (!wsu::encoderStart()) return;
    wsu::netStart();
}

ON_APPLICATION_ENDS() {
    wsu::netStop();
    wsu::encoderStop();
    wsu::captureShutdown();
    wsu::audioShutdown();
}
