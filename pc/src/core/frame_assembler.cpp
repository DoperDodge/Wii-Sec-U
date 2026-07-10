// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "core/frame_assembler.h"

#include <cstring>

namespace wsu {

namespace {
// frameId comparison that survives wraparound.
bool idNewer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}
} // namespace

void FrameAssembler::feed(const WsuVideoHeader &v, const uint8_t *payload) {
    stats_.slicesIn++;

    // A slice for a frame at or before the newest delivered one is stale.
    if (anyDelivered_ && !idNewer(v.frameId, newestDelivered_)) {
        return;
    }

    Pending *p = findOrAllocate(v);
    if (p == nullptr) return;

    if (v.sliceIndex >= p->sliceCount || p->haveSlice[v.sliceIndex]) {
        return; // corrupt index or duplicate
    }
    std::memcpy(p->data.data() +
                    static_cast<size_t>(v.sliceIndex) * WSU_MAX_SLICE_PAYLOAD,
                payload, v.payloadLen);
    p->sliceLen[v.sliceIndex] = v.payloadLen;
    p->haveSlice[v.sliceIndex] = true;
    p->slicesHave++;

    if (p->slicesHave == p->sliceCount) {
        deliver(*p);
    }
}

FrameAssembler::Pending *FrameAssembler::findOrAllocate(
    const WsuVideoHeader &v) {
    Pending *freeSlot = nullptr;
    Pending *oldest = nullptr;
    for (Pending &p : pending_) {
        if (p.inUse && p.frameId == v.frameId) {
            return &p;
        }
        if (!p.inUse && freeSlot == nullptr) {
            freeSlot = &p;
        }
        if (p.inUse && (oldest == nullptr || idNewer(oldest->frameId,
                                                     p.frameId))) {
            oldest = &p;
        }
    }

    Pending *slot = freeSlot;
    if (slot == nullptr) {
        // Window full: evict the oldest incomplete frame, but never to make
        // room for a frame older than everything pending.
        if (oldest != nullptr && idNewer(v.frameId, oldest->frameId)) {
            stats_.framesDropped++;
            slot = oldest;
        } else {
            return nullptr;
        }
    }

    slot->inUse = true;
    slot->frameId = v.frameId;
    slot->timestampMs = v.timestampMs;
    slot->flags = v.flags;
    slot->sliceCount = v.sliceCount;
    slot->slicesHave = 0;
    slot->haveSlice.assign(v.sliceCount, false);
    slot->data.assign(static_cast<size_t>(v.sliceCount) *
                          WSU_MAX_SLICE_PAYLOAD,
                      0);
    slot->sliceLen.assign(v.sliceCount, 0);
    return slot;
}

void FrameAssembler::deliver(Pending &p) {
    AssembledFrame frame;
    frame.frameId = p.frameId;
    frame.timestampMs = p.timestampMs;
    frame.flags = p.flags;

    size_t total = 0;
    for (uint16_t i = 0; i < p.sliceCount; i++) total += p.sliceLen[i];
    frame.data.reserve(total);
    for (uint16_t i = 0; i < p.sliceCount; i++) {
        const uint8_t *src =
            p.data.data() + static_cast<size_t>(i) * WSU_MAX_SLICE_PAYLOAD;
        frame.data.insert(frame.data.end(), src, src + p.sliceLen[i]);
    }

    newestDelivered_ = p.frameId;
    anyDelivered_ = true;
    p.inUse = false;

    // Abandon anything now older than the delivered frame.
    for (Pending &q : pending_) {
        if (q.inUse && !idNewer(q.frameId, newestDelivered_)) {
            q.inUse = false;
            stats_.framesDropped++;
        }
    }

    stats_.framesCompleted++;
    stats_.bytesCompleted += frame.data.size();
    if (onFrame_) onFrame_(std::move(frame));
}

} // namespace wsu
