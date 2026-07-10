// FrameAssembler reassembly-policy tests.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <vector>

#include "core/frame_assembler.h"
#include "test_common.h"

using namespace wsu;

namespace {

// Builds the slice header for frame `id` covering `data`.
WsuVideoHeader sliceHeader(uint32_t id, uint16_t index, uint16_t count,
                           uint16_t len) {
    WsuVideoHeader v{};
    v.frameId = id;
    v.timestampMs = 1000 + id;
    v.flags = WSU_VIDEO_FLAG_KEYFRAME;
    v.sliceIndex = index;
    v.sliceCount = count;
    v.payloadLen = len;
    return v;
}

std::vector<uint8_t> makeFrameData(uint32_t id, size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>((id * 31 + i) & 0xFF);
    }
    return data;
}

// Feeds frame `id` (split into slices of WSU_MAX_SLICE_PAYLOAD) in the
// given slice order; -1 in `order` skips that slice.
void feedFrame(FrameAssembler &fa, uint32_t id,
               const std::vector<uint8_t> &data,
               const std::vector<int> &order) {
    uint16_t count = wsu_video_slice_count(static_cast<uint32_t>(data.size()));
    for (int idx : order) {
        if (idx < 0) continue;
        size_t off = static_cast<size_t>(idx) * WSU_MAX_SLICE_PAYLOAD;
        size_t remain = data.size() - off;
        uint16_t len = static_cast<uint16_t>(
            remain < WSU_MAX_SLICE_PAYLOAD ? remain : WSU_MAX_SLICE_PAYLOAD);
        WsuVideoHeader v =
            sliceHeader(id, static_cast<uint16_t>(idx), count, len);
        fa.feed(v, data.data() + off);
    }
}

} // namespace

int main() {
    std::vector<AssembledFrame> got;
    FrameAssembler fa([&](AssembledFrame &&f) { got.push_back(std::move(f)); });

    // 1. Single-slice frame.
    auto f0 = makeFrameData(0, 500);
    feedFrame(fa, 0, f0, {0});
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0].data == f0);
    CHECK_EQ(got[0].frameId, 0u);
    CHECK_EQ(got[0].timestampMs, 1000u);

    // 2. Multi-slice frame delivered out of order.
    auto f1 = makeFrameData(1, 3 * WSU_MAX_SLICE_PAYLOAD + 17);
    feedFrame(fa, 1, f1, {3, 1, 0, 2});
    CHECK_EQ(got.size(), 2u);
    CHECK(got[1].data == f1);

    // 3. Duplicate slices are harmless.
    auto f2 = makeFrameData(2, 2 * WSU_MAX_SLICE_PAYLOAD);
    feedFrame(fa, 2, f2, {0, 0, 1, 1});
    CHECK_EQ(got.size(), 3u);
    CHECK(got[2].data == f2);

    // 4. A frame with a missing slice is never delivered, and a newer
    //    complete frame supersedes it.
    auto f3 = makeFrameData(3, 2 * WSU_MAX_SLICE_PAYLOAD);
    feedFrame(fa, 3, f3, {0, -1}); // slice 1 lost
    auto f4 = makeFrameData(4, 400);
    feedFrame(fa, 4, f4, {0});
    CHECK_EQ(got.size(), 4u);
    CHECK_EQ(got[3].frameId, 4u);

    // 5. The straggler slice of the abandoned frame 3 must not resurrect it.
    feedFrame(fa, 3, f3, {1});
    CHECK_EQ(got.size(), 4u);

    // 6. Slices for frames older than the newest delivered are ignored.
    feedFrame(fa, 2, f2, {0, 1});
    CHECK_EQ(got.size(), 4u);

    // 7. Interleaved frames both complete.
    auto f5 = makeFrameData(5, 2 * WSU_MAX_SLICE_PAYLOAD);
    auto f6 = makeFrameData(6, 2 * WSU_MAX_SLICE_PAYLOAD);
    {
        WsuVideoHeader v = sliceHeader(5, 0, 2, WSU_MAX_SLICE_PAYLOAD);
        fa.feed(v, f5.data());
        v = sliceHeader(6, 0, 2, WSU_MAX_SLICE_PAYLOAD);
        fa.feed(v, f6.data());
        v = sliceHeader(5, 1, 2, WSU_MAX_SLICE_PAYLOAD);
        fa.feed(v, f5.data() + WSU_MAX_SLICE_PAYLOAD);
        v = sliceHeader(6, 1, 2, WSU_MAX_SLICE_PAYLOAD);
        fa.feed(v, f6.data() + WSU_MAX_SLICE_PAYLOAD);
    }
    CHECK_EQ(got.size(), 6u);
    CHECK_EQ(got[4].frameId, 5u);
    CHECK(got[4].data == f5);
    CHECK_EQ(got[5].frameId, 6u);
    CHECK(got[5].data == f6);

    // 8. Pending-window overflow evicts the oldest incomplete frame.
    for (uint32_t id = 10; id < 16; id++) {
        auto f = makeFrameData(id, 2 * WSU_MAX_SLICE_PAYLOAD);
        feedFrame(fa, id, f, {0}); // all incomplete
    }
    auto f16 = makeFrameData(16, 300);
    feedFrame(fa, 16, f16, {0});
    CHECK_EQ(got.size(), 7u);
    CHECK_EQ(got[6].frameId, 16u);

    CHECK(fa.stats().framesCompleted == 7);
    CHECK(fa.stats().framesDropped > 0);

    return testSummary("test_frame_assembler");
}
