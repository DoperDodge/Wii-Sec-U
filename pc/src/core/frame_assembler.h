// FrameAssembler — reassembles sliced VIDEO packets into whole frames.
//
// Frames arrive as up to sliceCount UDP slices, possibly out of order,
// possibly with gaps, possibly interleaved between consecutive frames.
// Policy is "latest wins" (PLAN.md §5): once a newer frame completes,
// older incomplete frames are abandoned. Incomplete frames also age out
// when the pending window fills.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "wsu_protocol.h"

namespace wsu {

// A fully reassembled frame handed to the consumer.
struct AssembledFrame {
    uint32_t frameId = 0;
    uint32_t timestampMs = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> data;
};

struct AssemblerStats {
    uint64_t slicesIn = 0;
    uint64_t framesCompleted = 0;
    uint64_t framesDropped = 0; // abandoned incomplete or arrived stale
    uint64_t bytesCompleted = 0;
};

class FrameAssembler {
  public:
    using FrameCallback = std::function<void(AssembledFrame &&)>;

    explicit FrameAssembler(FrameCallback onFrame)
        : onFrame_(std::move(onFrame)) {}

    // Feeds one parsed VIDEO slice. Invokes the callback synchronously
    // when this slice completes a frame. Not thread-safe; call from one
    // receive thread.
    void feed(const WsuVideoHeader &v, const uint8_t *payload);

    const AssemblerStats &stats() const { return stats_; }

  private:
    struct Pending {
        bool inUse = false;
        uint32_t frameId = 0;
        uint32_t timestampMs = 0;
        uint8_t flags = 0;
        uint16_t sliceCount = 0;
        uint16_t slicesHave = 0;
        std::vector<bool> haveSlice;
        std::vector<uint8_t> data;      // sliceCount * WSU_MAX_SLICE_PAYLOAD
        std::vector<uint16_t> sliceLen; // actual length per slice
    };

    static constexpr int kMaxPending = 4;

    Pending *findOrAllocate(const WsuVideoHeader &v);
    void deliver(Pending &p);

    FrameCallback onFrame_;
    Pending pending_[kMaxPending];
    uint32_t newestDelivered_ = 0;
    bool anyDelivered_ = false;
    AssemblerStats stats_;
};

} // namespace wsu
