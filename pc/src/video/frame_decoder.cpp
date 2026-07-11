// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "video/frame_decoder.h"

#include <cstring>

#include "util/log.h"

// stb_image, JPEG only: the stream is MJPEG and every other format is
// dead weight (and attack surface) here.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace wsu {

bool FrameDecoder::decode(uint8_t codec, uint16_t configWidth,
                          uint16_t configHeight, const AssembledFrame &frame,
                          DecodedFrame &out) {
    switch (codec) {
    case WSU_CODEC_RAWRGB: {
        size_t expected =
            static_cast<size_t>(configWidth) * configHeight * 3;
        if (configWidth == 0 || configHeight == 0 ||
            frame.data.size() != expected) {
            failed_++;
            return false;
        }
        out.width = configWidth;
        out.height = configHeight;
        out.rgb = frame.data; // already tightly packed RGB24
        out.timestampMs = frame.timestampMs;
        out.frameId = frame.frameId;
        decoded_++;
        return true;
    }
    case WSU_CODEC_MJPEG: {
        int w = 0, h = 0, comp = 0;
        if (frame.data.size() >
            static_cast<size_t>(INT32_MAX)) { // stb takes int lengths
            failed_++;
            return false;
        }
        stbi_uc *pixels = stbi_load_from_memory(
            frame.data.data(), static_cast<int>(frame.data.size()), &w, &h,
            &comp, 3);
        if (pixels == nullptr || w <= 0 || h <= 0) {
            if (pixels != nullptr) stbi_image_free(pixels);
            failed_++;
            return false;
        }
        out.width = w;
        out.height = h;
        out.rgb.assign(pixels,
                       pixels + static_cast<size_t>(w) * h * 3);
        stbi_image_free(pixels);
        out.timestampMs = frame.timestampMs;
        out.frameId = frame.frameId;
        decoded_++;
        return true;
    }
    default:
        failed_++;
        return false;
    }
}

} // namespace wsu
