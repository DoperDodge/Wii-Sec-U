// FrameDecoder tests: JPEG round-trip (encoded with stb_image_write, the
// test-only counterpart of the console's libjpeg encoder), RAWRGB
// passthrough, and malformed-input rejection.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <cstdlib>
#include <vector>

#include "test_common.h"
#include "video/frame_decoder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

using namespace wsu;

namespace {

void appendBytes(void *ctx, void *data, int size) {
    auto *out = static_cast<std::vector<uint8_t> *>(ctx);
    const auto *bytes = static_cast<const uint8_t *>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// Solid-color RGB image → JPEG bytes.
std::vector<uint8_t> makeJpeg(int w, int h, uint8_t r, uint8_t g,
                              uint8_t b) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = r;
        rgb[i + 1] = g;
        rgb[i + 2] = b;
    }
    std::vector<uint8_t> jpeg;
    int ok = stbi_write_jpg_to_func(appendBytes, &jpeg, w, h, 3, rgb.data(),
                                    80);
    return ok != 0 ? jpeg : std::vector<uint8_t>{};
}

AssembledFrame frameOf(std::vector<uint8_t> data, uint32_t id) {
    AssembledFrame f;
    f.frameId = id;
    f.timestampMs = 1000 + id;
    f.flags = WSU_VIDEO_FLAG_KEYFRAME;
    f.data = std::move(data);
    return f;
}

} // namespace

int main() {
    FrameDecoder decoder;
    DecodedFrame out;

    // --- MJPEG: decode a real JPEG, verify dims and approximate color.
    auto jpeg = makeJpeg(64, 48, 200, 40, 90);
    CHECK(!jpeg.empty());
    CHECK(decoder.decode(WSU_CODEC_MJPEG, 0, 0, frameOf(jpeg, 1), out));
    CHECK_EQ(out.width, 64);
    CHECK_EQ(out.height, 48);
    CHECK_EQ(out.rgb.size(), 64u * 48u * 3u);
    CHECK_EQ(out.frameId, 1u);
    // JPEG is lossy; the center pixel of a solid image stays close.
    {
        size_t center = ((24u * 64u) + 32u) * 3u;
        CHECK(std::abs(out.rgb[center] - 200) < 16);
        CHECK(std::abs(out.rgb[center + 1] - 40) < 16);
        CHECK(std::abs(out.rgb[center + 2] - 90) < 16);
    }

    // --- MJPEG: a truncated JPEG may still decode (stb is deliberately
    // lenient — a torn frame beats a dropped one), but if it does, the
    // dimensions must be sane; garbage and empty payloads must fail.
    {
        auto truncated = jpeg;
        truncated.resize(truncated.size() / 3);
        DecodedFrame torn;
        if (decoder.decode(WSU_CODEC_MJPEG, 0, 0, frameOf(truncated, 2),
                           torn)) {
            CHECK_EQ(torn.width, 64);
            CHECK_EQ(torn.height, 48);
        }
        std::vector<uint8_t> garbage(500, 0xAB);
        CHECK(!decoder.decode(WSU_CODEC_MJPEG, 0, 0, frameOf(garbage, 3),
                              out));
        CHECK(!decoder.decode(WSU_CODEC_MJPEG, 0, 0, frameOf({}, 4), out));
    }

    // --- RAWRGB: exact passthrough when sizes agree.
    {
        std::vector<uint8_t> raw(16u * 9u * 3u);
        for (size_t i = 0; i < raw.size(); i++) {
            raw[i] = static_cast<uint8_t>(i * 5);
        }
        CHECK(decoder.decode(WSU_CODEC_RAWRGB, 16, 9, frameOf(raw, 5), out));
        CHECK_EQ(out.width, 16);
        CHECK_EQ(out.height, 9);
        CHECK(out.rgb == raw);

        // Size mismatch (wrong config or truncated frame) is rejected.
        CHECK(!decoder.decode(WSU_CODEC_RAWRGB, 32, 9, frameOf(raw, 6),
                              out));
        CHECK(!decoder.decode(WSU_CODEC_RAWRGB, 0, 0, frameOf(raw, 7),
                              out));
    }

    // --- Unknown codec is rejected.
    CHECK(!decoder.decode(99, 16, 9, frameOf({1, 2, 3}, 8), out));

    // Deterministic outcomes only (the truncated-JPEG case may land on
    // either counter depending on stb's tolerance).
    CHECK(decoder.decoded() >= 2);
    CHECK(decoder.failed() >= 5);

    return testSummary("test_frame_decoder");
}
