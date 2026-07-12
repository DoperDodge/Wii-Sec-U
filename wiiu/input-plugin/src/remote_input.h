// Shared state between the wsu-input network thread (producer) and the
// VPAD/KPAD read hooks (consumers, called on game threads).
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <coreinit/mutex.h>

#include "wsu_protocol.h"

namespace wsu {

class RemoteInput {
  public:
    void init() {
        OSInitMutex(&mutex_);
        for (int i = 0; i < WSU_MAX_PLAYERS; i++) {
            wsu_input_state_init(&state_[i], static_cast<uint8_t>(i));
            updatedAtMs_[i] = 0;
        }
    }

    // Called by the network thread for every state in an INPUT_BUNDLE.
    void update(const WsuInputState &s, uint32_t nowMs) {
        if (s.slot >= WSU_MAX_PLAYERS) return;
        OSLockMutex(&mutex_);
        state_[s.slot] = s;
        updatedAtMs_[s.slot] = nowMs;
        OSUnlockMutex(&mutex_);
    }

    // Called by injection hooks. Returns true and copies the state when
    // the slot received an update within WSU_INPUT_TIMEOUT_MS — a silent
    // host self-heals to "controller neutral / not present".
    bool get(uint8_t slot, uint32_t nowMs, WsuInputState *out) {
        if (slot >= WSU_MAX_PLAYERS) return false;
        bool active = false;
        OSLockMutex(&mutex_);
        if (updatedAtMs_[slot] != 0 &&
            (nowMs - updatedAtMs_[slot]) < WSU_INPUT_TIMEOUT_MS) {
            *out = state_[slot];
            active = true;
        }
        OSUnlockMutex(&mutex_);
        return active;
    }

    void deactivateAll() {
        OSLockMutex(&mutex_);
        for (int i = 0; i < WSU_MAX_PLAYERS; i++) updatedAtMs_[i] = 0;
        OSUnlockMutex(&mutex_);
    }

  private:
    OSMutex mutex_{};
    WsuInputState state_[WSU_MAX_PLAYERS]{};
    uint32_t updatedAtMs_[WSU_MAX_PLAYERS]{};
};

// Defined in main.cpp; used by injection.cpp.
extern RemoteInput gRemoteInput;

// True when injection for `slot` is allowed by the config menu (master
// switch AND the per-player toggle). Defined in main.cpp.
bool slotInjectionEnabled(uint8_t slot);

} // namespace wsu
