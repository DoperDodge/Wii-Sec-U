// Monotonic millisecond clock shared by the whole PC app.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace wsu {

// Milliseconds from a steady (monotonic) clock. Truncated to 32 bits to
// match the protocol's timestamp width; wraps after ~49 days, which every
// consumer treats as opaque "recent vs stale" comparison material.
inline uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count());
}

// Age of `thenMs` relative to `nowMs`, wrap-safe for spans < 2^31 ms.
inline uint32_t ageMs(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs;
}

inline void sleepMs(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace wsu
