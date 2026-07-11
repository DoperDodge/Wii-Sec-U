// FrameDecoder — turns an AssembledFrame into RGB24 pixels.
//
// Handles both stream codecs: WSU_CODEC_MJPEG (one baseline JPEG per
// frame, decoded with the vendored stb_image) and WSU_CODEC_RAWRGB (the
// console simulator's uncompressed test pattern, validated passthrough).
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdint>
#include <vector>

#include "core/frame_assembler.h"

namespace wsu {

// A decoded frame. `rgb` is tightly packed RGB24, row-major, top-down.
struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
    uint32_t timestampMs = 0;
    uint32_t frameId = 0;
};

class FrameDecoder {
  public:
    // Decodes according to `codec` (WsuVideoCodec). For RAWRGB the config
    // dimensions are required to validate/shape the payload; for MJPEG the
    // dimensions come from the JPEG itself. Returns false (and leaves
    // `out` untouched) on malformed input — a corrupt frame is dropped,
    // never fatal.
    bool decode(uint8_t codec, uint16_t configWidth, uint16_t configHeight,
                const AssembledFrame &frame, DecodedFrame &out);

    uint64_t decoded() const { return decoded_; }
    uint64_t failed() const { return failed_; }

  private:
    uint64_t decoded_ = 0;
    uint64_t failed_ = 0;
};

} // namespace wsu
