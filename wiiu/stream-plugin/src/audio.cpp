// TV audio capture via the AX device final-mix callback (PLAN.md §4A.3).
//
// AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, ...) hands us every
// mixed TV audio block before output. The callback runs on the AX audio
// frame thread every 3 ms, so it must stay tiny: clamp to int16, take the
// first two channels, push into a lock-free SPSC ring buffer. The
// wsu-stream net thread drains the ring into AUDIO packets.
//
// wut declares the callback parameter as void*; the actual layout is the
// well-known AXDeviceFinalMixData (verified against decaf-emu's sndcore2
// implementation):
//   0x0  int32_t **data      per-channel sample buffers
//   0x4  uint16_t channels   channel count per device
//   0x6  uint16_t samples    samples per channel in this block
//   0x8  uint16_t numDevices
//   0xa  uint16_t channelsOut
//
// A previously registered callback (rare, but possible) is chained.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>
#include <cstring>

#include <sndcore2/core.h>
#include <sndcore2/device.h>

#include "stream_state.h"
#include "wsu_wiiu.h"

namespace wsu {
namespace {

struct AXDeviceFinalMixData {
    int32_t **data;
    uint16_t channels;
    uint16_t samples;
    uint16_t numDevices;
    uint16_t channelsOut;
};

// Single-producer (AX frame thread) / single-consumer (net thread) ring
// of interleaved stereo pairs. 8192 pairs ≈ 170 ms at 48 kHz.
constexpr size_t kRingPairs = 8192;
int16_t gRing[kRingPairs * 2];
std::atomic<uint32_t> gHead{0}; // write index (pairs), producer-owned
std::atomic<uint32_t> gTail{0}; // read index (pairs), consumer-owned

std::atomic<bool> gEnabled{false};
std::atomic<bool> gRegistered{false};
AXDeviceFinalMixCallback gChainedCallback = nullptr;

int16_t clampSample(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

void finalMixCallback(void *rawData) {
    if (gChainedCallback != nullptr) {
        gChainedCallback(rawData);
    }
    if (!gEnabled.load(std::memory_order_relaxed) || rawData == nullptr) {
        return;
    }

    const auto *mix = static_cast<const AXDeviceFinalMixData *>(rawData);
    if (mix->data == nullptr || mix->channels == 0 || mix->samples == 0) {
        return;
    }

    const int32_t *left = mix->data[0];
    const int32_t *right = mix->channels >= 2 ? mix->data[1] : mix->data[0];
    if (left == nullptr || right == nullptr) return;

    uint32_t head = gHead.load(std::memory_order_relaxed);
    uint32_t tail = gTail.load(std::memory_order_acquire);
    uint32_t free = kRingPairs - (head - tail);

    uint32_t n = mix->samples;
    if (n > free) n = free; // overrun: drop the newest samples

    for (uint32_t i = 0; i < n; i++) {
        size_t idx = ((head + i) % kRingPairs) * 2;
        gRing[idx] = clampSample(left[i]);
        gRing[idx + 1] = clampSample(right[i]);
    }
    gHead.store(head + n, std::memory_order_release);
}

} // namespace

size_t audioRead(int16_t *out, size_t maxPairs) {
    uint32_t tail = gTail.load(std::memory_order_relaxed);
    uint32_t head = gHead.load(std::memory_order_acquire);
    uint32_t avail = head - tail;
    uint32_t n = avail < maxPairs ? avail : static_cast<uint32_t>(maxPairs);

    for (uint32_t i = 0; i < n; i++) {
        size_t idx = ((tail + i) % kRingPairs) * 2;
        out[i * 2] = gRing[idx];
        out[i * 2 + 1] = gRing[idx + 1];
    }
    gTail.store(tail + n, std::memory_order_release);
    return n;
}

void audioEnsureRegistered() {
    if (!gConfig.audio || gRegistered.load()) return;
    // AX is initialized by the game, not by us — poll until it is.
    if (!AXIsInit()) return;

    AXDeviceFinalMixCallback previous = nullptr;
    AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, &previous);
    if (previous == &finalMixCallback) {
        gRegistered.store(true);
        return;
    }
    gChainedCallback = previous;
    if (AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV,
                                         &finalMixCallback) == AX_RESULT_SUCCESS) {
        gRegistered.store(true);
        gEnabled.store(true);
        WSU_LOG("stream: TV audio tap registered");
    }
}

void audioInit() {
    gHead.store(0);
    gTail.store(0);
    gRegistered.store(false);
    gEnabled.store(false);
    gChainedCallback = nullptr;
}

void audioShutdown() {
    gEnabled.store(false);
    // Restore the chained callback if we are still the registered one.
    if (gRegistered.load() && AXIsInit()) {
        AXDeviceFinalMixCallback current = nullptr;
        AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, &current);
        if (current == &finalMixCallback) {
            AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV,
                                             gChainedCallback);
        }
    }
    gRegistered.store(false);
}

} // namespace wsu
