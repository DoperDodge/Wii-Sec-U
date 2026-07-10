// Canonical controller state used throughout the PC app. The wire form is
// WsuInputState (protocol/wsu_protocol.h); this adds host-side metadata.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdint>

#include "wsu_protocol.h"

namespace wsu {

// One controller's full state plus the local time it was last refreshed.
struct ControllerState {
    WsuInputState wire{};
    uint32_t updatedAtMs = 0;

    ControllerState() { wsu_input_state_init(&wire, WSU_NO_SLOT); }

    static ControllerState neutral(uint8_t slot) {
        ControllerState s;
        wsu_input_state_init(&s.wire, slot);
        return s;
    }
};

// Clamp helper for float [-1,1] stick values into wire i16 range.
inline int16_t stickToWire(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * WSU_STICK_MAX);
}

inline float stickFromWire(int16_t v) {
    return static_cast<float>(v) / static_cast<float>(WSU_STICK_MAX);
}

} // namespace wsu
