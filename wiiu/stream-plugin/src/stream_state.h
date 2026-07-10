// Shared state and cross-module interfaces of the wsu-stream plugin.
//
// Module map (one UDP socket, three producers):
//   net.cpp     — socket, host handshake/keepalive, CONFIG/AUDIO sending
//   capture.cpp — GX2 scan-out hook, ping-pong capture surfaces
//   encoder.cpp — encoder thread: downscale → JPEG → wsuNetSendVideoFrame
//   audio.cpp   — AX final-mix tap → ring buffer (drained by net thread)
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstddef>
#include <cstdint>

namespace wsu {

struct StreamConfig {
    int width = 428;   // encoded output size
    int height = 240;
    int fps = 20;      // capture/encode rate cap
    int quality = 60;  // JPEG quality 1..100
    int port = 0;      // filled from WSU_CONSOLE_STREAM_PORT or config file
    bool audio = true; // tap and stream TV audio (PCM16)
};

extern StreamConfig gConfig;

// ---- net.cpp ----
bool netStart();
void netStop();
// True while a host is connected and fresh; capture/encode idle otherwise.
bool netHostActive();
// Slices one encoded frame into VIDEO datagrams and sends them.
void netSendVideoFrame(const uint8_t *data, size_t len, uint32_t timestampMs,
                       bool keyframe);

// ---- capture.cpp ----
bool captureInit();
void captureShutdown();

// ---- encoder.cpp ----
bool encoderStart();
void encoderStop();
// Called from the GX2 hook when a detiled RGBA capture is CPU-readable.
// `pitch` is in pixels; `format` is the GX2SurfaceFormat value.
void encoderSubmit(int surfaceIndex);
// Encoder → capture: surface is free again.
void captureRelease(int surfaceIndex);

// Capture surface data the encoder reads (set by capture.cpp).
struct CapturedSurface {
    const uint8_t *pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;  // pixels per row
    uint32_t format = 0; // GX2SurfaceFormat
    uint32_t timestampMs = 0;
};
const CapturedSurface *captureGetSurface(int surfaceIndex);

// ---- audio.cpp ----
void audioInit();
void audioShutdown();
// Ensures the final-mix tap is registered once AX is initialized; called
// periodically from the net thread.
void audioEnsureRegistered();
// Pops up to `maxPairs` interleaved stereo sample pairs; returns pairs read.
size_t audioRead(int16_t *out, size_t maxPairs);

} // namespace wsu
