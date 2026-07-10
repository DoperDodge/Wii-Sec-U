// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "core/input_hub.h"

#include "util/time_util.h"

namespace wsu {

void InputHub::set(uint8_t slot, const WsuInputState &state, uint32_t nowMs) {
    if (slot >= WSU_MAX_PLAYERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot].wire = state;
    slots_[slot].wire.slot = slot; // slot is authoritative, not client-claimed
    slots_[slot].updatedAtMs = nowMs;
}

void InputHub::clear(uint8_t slot) {
    if (slot >= WSU_MAX_PLAYERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot] = ControllerState::neutral(slot);
    slots_[slot].updatedAtMs = 0;
}

int InputHub::packBundle(uint8_t *buf, size_t cap, uint16_t seq,
                         uint32_t nowMs) const {
    WsuInputState active[WSU_MAX_PLAYERS];
    uint8_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint8_t i = 0; i < WSU_MAX_PLAYERS; i++) {
            const ControllerState &s = slots_[i];
            if (s.updatedAtMs != 0 &&
                ageMs(nowMs, s.updatedAtMs) < WSU_INPUT_TIMEOUT_MS) {
                active[count++] = s.wire;
            }
        }
    }
    if (count == 0) return 0;
    int n = wsu_pack_input_bundle(buf, cap, seq, nowMs, active, count);
    return n < 0 ? 0 : n;
}

bool InputHub::isActive(uint8_t slot, uint32_t nowMs) const {
    if (slot >= WSU_MAX_PLAYERS) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const ControllerState &s = slots_[slot];
    return s.updatedAtMs != 0 &&
           ageMs(nowMs, s.updatedAtMs) < WSU_INPUT_TIMEOUT_MS;
}

WsuInputState InputHub::get(uint8_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= WSU_MAX_PLAYERS) {
        WsuInputState s;
        wsu_input_state_init(&s, WSU_NO_SLOT);
        return s;
    }
    return slots_[slot].wire;
}

} // namespace wsu
