// Encoder thread: nearest-neighbor downscale to the configured output
// size, JPEG-compress (libjpeg-turbo portlib, plain C path on Espresso —
// no SIMD), then hand the frame to net.cpp for slicing (PLAN.md §4A.2).
//
// Runs pinned to CPU core 2 at low priority; when it can't keep up the
// capture hook simply skips frames (both surfaces busy), which is the
// intended degradation: game speed > stream smoothness.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>
#include <cstring>

#include <coreinit/messagequeue.h>
#include <gx2/enum.h>

#include <cstdio> // jpeglib.h uses FILE but doesn't include stdio itself
#include <jpeglib.h>

#include "stream_state.h"
#include "wsu_wiiu.h"

namespace wsu {
namespace {

constexpr uint32_t kEncoderStackSize = 512 * 1024; // libjpeg needs room
constexpr int32_t kEncoderPriority = 28;           // below net + game
constexpr int kQueueSize = 4;
constexpr int32_t kMsgShutdown = -1;

std::atomic<bool> gRunning{false};
OSMessageQueue gQueue;
OSMessage gQueueBuf[kQueueSize];
PluginThread gThread;

uint8_t *gRgb = nullptr;           // downscaled RGB24 output
unsigned char *gJpegBuf = nullptr; // preallocated JPEG output buffer
unsigned long gJpegCap = 0;        // its fixed capacity

// Downscales one captured RGBA surface into gRgb (RGB24, outW×outH).
void downscale(const CapturedSurface &src, int outW, int outH) {
    const uint32_t *pixels =
        reinterpret_cast<const uint32_t *>(src.pixels);
    for (int y = 0; y < outH; y++) {
        uint32_t srcY = static_cast<uint32_t>(y) * src.height /
                        static_cast<uint32_t>(outH);
        const uint32_t *row = pixels + static_cast<size_t>(srcY) * src.pitch;
        uint8_t *out = gRgb + static_cast<size_t>(y) * outW * 3;
        for (int x = 0; x < outW; x++) {
            uint32_t srcX = static_cast<uint32_t>(x) * src.width /
                            static_cast<uint32_t>(outW);
            uint32_t px = row[srcX]; // big-endian load: R is the MSB
            out[x * 3 + 0] = static_cast<uint8_t>(px >> 24);
            out[x * 3 + 1] = static_cast<uint8_t>(px >> 16);
            out[x * 3 + 2] = static_cast<uint8_t>(px >> 8);
        }
    }
}

// JPEG-compresses gRgb into gJpegBuf; returns encoded size, 0 on failure.
//
// jpeg_mem_dest is handed our fixed buffer with its full capacity each
// frame (passing the previous frame's *actual* size back in would make
// libjpeg believe the buffer shrank and trigger leaky reallocation). The
// capacity is w*h*3 so an overflow-triggered replacement buffer is
// practically impossible, but it's still handled.
size_t compress(int w, int h, int quality) {
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *buf = gJpegBuf;
    unsigned long size = gJpegCap;
    jpeg_mem_dest(&cinfo, &buf, &size);

    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.dct_method = JDCT_IFAST; // cheapest DCT; quality cost is fine
                                   // at streaming bitrates

    jpeg_start_compress(&cinfo, TRUE);
    JSAMPROW row;
    while (cinfo.next_scanline < cinfo.image_height) {
        row = gRgb + static_cast<size_t>(cinfo.next_scanline) * w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    if (buf != gJpegBuf) {
        // Output outgrew our buffer and libjpeg allocated a replacement:
        // copy what fits and drop the rest of this frame.
        size_t copied = size < gJpegCap ? size : 0;
        if (copied > 0) std::memcpy(gJpegBuf, buf, copied);
        free(buf);
        return copied;
    }
    return size;
}

int encoderThread(int, const char **) {
    // Buffers sized for the largest resolution preset so the config menu
    // can switch presets while streaming.
    constexpr int kMaxOutW = 854;
    constexpr int kMaxOutH = 480;

    gRgb = new (std::nothrow)
        uint8_t[static_cast<size_t>(kMaxOutW) * kMaxOutH * 3];
    gJpegCap = static_cast<unsigned long>(kMaxOutW) * kMaxOutH * 3;
    gJpegBuf = static_cast<unsigned char *>(malloc(gJpegCap));
    if (gRgb == nullptr || gJpegBuf == nullptr) {
        WSU_LOG("stream: encoder buffer alloc failed");
        delete[] gRgb;
        gRgb = nullptr;
        free(gJpegBuf);
        gJpegBuf = nullptr;
        return -1;
    }

    while (gRunning.load()) {
        OSMessage msg;
        OSReceiveMessage(&gQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        int32_t index = static_cast<int32_t>(msg.args[0]);
        if (index == kMsgShutdown) break;

        const CapturedSurface *src = captureGetSurface(index);
        if (src == nullptr || src->pixels == nullptr) {
            captureRelease(index);
            continue;
        }

        // Snapshot the live-tunable settings once per frame.
        int outW = gConfig.width;
        int outH = gConfig.height;
        if (outW < 16 || outW > kMaxOutW) outW = kMaxOutW;
        if (outH < 16 || outH > kMaxOutH) outH = kMaxOutH;

        downscale(*src, outW, outH);
        size_t jpegSize = compress(outW, outH, gConfig.quality);
        captureRelease(index);

        if (jpegSize > 0) {
            // MJPEG: every frame is independently decodable.
            netSendVideoFrame(gJpegBuf, jpegSize, src->timestampMs, true);
        }
    }

    delete[] gRgb;
    gRgb = nullptr;
    free(gJpegBuf);
    gJpegBuf = nullptr;
    gJpegCap = 0;
    return 0;
}

} // namespace

void encoderSubmit(int surfaceIndex) {
    OSMessage msg{};
    msg.args[0] = static_cast<uint32_t>(surfaceIndex);
    // Non-blocking: if the queue is full the frame is skipped and the
    // surface must be released so capture can reuse it.
    if (!OSSendMessage(&gQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
        captureRelease(surfaceIndex);
    }
}

bool encoderStart() {
    if (gRunning.load()) return true;
    OSInitMessageQueue(&gQueue, gQueueBuf, kQueueSize);
    gRunning.store(true);
    if (!gThread.start(encoderThread, kEncoderStackSize, kEncoderPriority,
                       OS_THREAD_ATTRIB_AFFINITY_CPU2, "wsu-stream-enc")) {
        WSU_LOG("stream: failed to start encoder thread");
        gRunning.store(false);
        return false;
    }
    return true;
}

void encoderStop() {
    if (!gRunning.exchange(false)) return;
    OSMessage msg{};
    msg.args[0] = static_cast<uint32_t>(kMsgShutdown);
    OSSendMessage(&gQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
    gThread.join();
}

} // namespace wsu
