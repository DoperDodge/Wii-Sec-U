// InputHub — the host's merge point for up to four controllers.
//
// Slot 0 is the host's local controller; slots 1..3 are fed by remote
// clients (SessionServer). At every tick the host packs the currently
// active slots into one INPUT_BUNDLE for the console. Slots go inactive
// after WSU_INPUT_TIMEOUT_MS without an update, so a vanished client's
// controller self-neutralizes instead of sticking.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "core/controller_state.h"

namespace wsu {

class InputHub {
  public:
    // Records a fresh full state for `slot` (0..3). Out-of-range slots are
    // ignored. `nowMs` is the local monotonic time of the update.
    void set(uint8_t slot, const WsuInputState &state, uint32_t nowMs);

    // Marks a slot inactive immediately (client disconnected).
    void clear(uint8_t slot);

    // Packs an INPUT_BUNDLE datagram with every active slot into `buf`.
    // Returns the datagram size, or 0 if no slot is active.
    int packBundle(uint8_t *buf, size_t cap, uint16_t seq,
                   uint32_t nowMs) const;

    // True if `slot` has been updated within the input timeout.
    bool isActive(uint8_t slot, uint32_t nowMs) const;

    // Snapshot of one slot's last state (for UI/diagnostics).
    WsuInputState get(uint8_t slot) const;

  private:
    mutable std::mutex mutex_;
    std::array<ControllerState, WSU_MAX_PLAYERS> slots_{};
};

} // namespace wsu
