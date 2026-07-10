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
// Config (optional): sd:/wiiu/wsu-stream.cfg, key=value per line:
//   width=428 height=240 fps=20 quality=60 port=4406 audio=1
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <wups.h>

#include "stream_state.h"
#include "wsu_protocol.h"
#include "wsu_wiiu.h"

WUPS_PLUGIN_NAME("wsu-stream");
WUPS_PLUGIN_DESCRIPTION("Wii-Sec-U: TV video/audio streaming to the host PC");
WUPS_PLUGIN_VERSION("v0.1.0");
WUPS_PLUGIN_AUTHOR("Wii-Sec-U contributors");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_WUT_MALLOC();
WUPS_USE_WUT_SOCKETS();

namespace wsu {

StreamConfig gConfig;

namespace {

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void loadConfig() {
    ConfigFile file("fs:/vol/external01/wiiu/wsu-stream.cfg");
    StreamConfig c;
    c.width = clampInt(file.getInt("width", c.width), 160, 854);
    c.height = clampInt(file.getInt("height", c.height), 90, 480);
    c.fps = clampInt(file.getInt("fps", c.fps), 1, 60);
    c.quality = clampInt(file.getInt("quality", c.quality), 1, 100);
    c.port = file.getInt("port", WSU_CONSOLE_STREAM_PORT);
    c.audio = file.getInt("audio", 1) != 0;
    gConfig = c;
    WSU_LOG("wsu-stream config: %dx%d@%dfps q%d port %d audio %d",
            c.width, c.height, c.fps, c.quality, c.port, c.audio ? 1 : 0);
}

} // namespace
} // namespace wsu

INITIALIZE_PLUGIN() {
    wsu::logInit();
    wsu::loadConfig();
}

ON_APPLICATION_START() {
    wsu::logInit();
    wsu::loadConfig();
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
