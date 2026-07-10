// Controller injection via WUPS function patching (PLAN.md §4A.5).
//
// Slot 0 → GamePad: VPADRead is hooked and the remote state is merged
//   over whatever the real GamePad reports (buttons OR-ed, sticks by
//   larger deflection, touch injected when the remote is touching).
// Slots 1-3 → Pro Controllers on WPAD channels 0-2: WPADProbe is hooked
//   to report a connected Pro Controller and KPADRead/KPADReadEx are
//   hooked to synthesize KPADStatus samples.
//
// trigger/release edges are recomputed against the previous *merged*
// state per channel so remote button presses generate proper press/release
// events even when packets repeat state at 120 Hz.
//
// Known gaps (docs/limitations.md): no connect-callback emulation yet
// (games that wait for WPADSetConnectCallback events instead of polling
// WPADProbe won't see P2-P4), no rumble return path, no motion for P2-P4.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <cstring>

#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <wups.h>

#include "remote_input.h"
#include "wsu_wiiu.h"

namespace wsu {
namespace {

// ------------------------------------------------------------------
// Canonical → VPAD (GamePad)
// ------------------------------------------------------------------

uint32_t mapButtonsToVpad(uint32_t b) {
    uint32_t v = 0;
    if (b & WSU_BTN_A) v |= VPAD_BUTTON_A;
    if (b & WSU_BTN_B) v |= VPAD_BUTTON_B;
    if (b & WSU_BTN_X) v |= VPAD_BUTTON_X;
    if (b & WSU_BTN_Y) v |= VPAD_BUTTON_Y;
    if (b & WSU_BTN_UP) v |= VPAD_BUTTON_UP;
    if (b & WSU_BTN_DOWN) v |= VPAD_BUTTON_DOWN;
    if (b & WSU_BTN_LEFT) v |= VPAD_BUTTON_LEFT;
    if (b & WSU_BTN_RIGHT) v |= VPAD_BUTTON_RIGHT;
    if (b & WSU_BTN_L) v |= VPAD_BUTTON_L;
    if (b & WSU_BTN_R) v |= VPAD_BUTTON_R;
    if (b & WSU_BTN_ZL) v |= VPAD_BUTTON_ZL;
    if (b & WSU_BTN_ZR) v |= VPAD_BUTTON_ZR;
    if (b & WSU_BTN_PLUS) v |= VPAD_BUTTON_PLUS;
    if (b & WSU_BTN_MINUS) v |= VPAD_BUTTON_MINUS;
    if (b & WSU_BTN_HOME) v |= VPAD_BUTTON_HOME;
    if (b & WSU_BTN_STICK_L) v |= VPAD_BUTTON_STICK_L;
    if (b & WSU_BTN_STICK_R) v |= VPAD_BUTTON_STICK_R;
    if (b & WSU_BTN_TV) v |= VPAD_BUTTON_TV;
    return v;
}

// Stick-emulation bits some games read instead of the analog values.
uint32_t vpadStickEmulation(float lx, float ly, float rx, float ry) {
    constexpr float kThreshold = 0.5f;
    uint32_t v = 0;
    if (lx < -kThreshold) v |= VPAD_STICK_L_EMULATION_LEFT;
    if (lx > kThreshold) v |= VPAD_STICK_L_EMULATION_RIGHT;
    if (ly > kThreshold) v |= VPAD_STICK_L_EMULATION_UP;
    if (ly < -kThreshold) v |= VPAD_STICK_L_EMULATION_DOWN;
    if (rx < -kThreshold) v |= VPAD_STICK_R_EMULATION_LEFT;
    if (rx > kThreshold) v |= VPAD_STICK_R_EMULATION_RIGHT;
    if (ry > kThreshold) v |= VPAD_STICK_R_EMULATION_UP;
    if (ry < -kThreshold) v |= VPAD_STICK_R_EMULATION_DOWN;
    return v;
}

// ------------------------------------------------------------------
// Canonical → WPAD Pro Controller
// ------------------------------------------------------------------

uint32_t mapButtonsToPro(uint32_t b) {
    uint32_t v = 0;
    if (b & WSU_BTN_A) v |= WPAD_PRO_BUTTON_A;
    if (b & WSU_BTN_B) v |= WPAD_PRO_BUTTON_B;
    if (b & WSU_BTN_X) v |= WPAD_PRO_BUTTON_X;
    if (b & WSU_BTN_Y) v |= WPAD_PRO_BUTTON_Y;
    if (b & WSU_BTN_UP) v |= WPAD_PRO_BUTTON_UP;
    if (b & WSU_BTN_DOWN) v |= WPAD_PRO_BUTTON_DOWN;
    if (b & WSU_BTN_LEFT) v |= WPAD_PRO_BUTTON_LEFT;
    if (b & WSU_BTN_RIGHT) v |= WPAD_PRO_BUTTON_RIGHT;
    if (b & WSU_BTN_L) v |= WPAD_PRO_TRIGGER_L;
    if (b & WSU_BTN_R) v |= WPAD_PRO_TRIGGER_R;
    if (b & WSU_BTN_ZL) v |= WPAD_PRO_TRIGGER_ZL;
    if (b & WSU_BTN_ZR) v |= WPAD_PRO_TRIGGER_ZR;
    if (b & WSU_BTN_PLUS) v |= WPAD_PRO_BUTTON_PLUS;
    if (b & WSU_BTN_MINUS) v |= WPAD_PRO_BUTTON_MINUS;
    if (b & WSU_BTN_HOME) v |= WPAD_PRO_BUTTON_HOME;
    if (b & WSU_BTN_STICK_L) v |= WPAD_PRO_BUTTON_STICK_L;
    if (b & WSU_BTN_STICK_R) v |= WPAD_PRO_BUTTON_STICK_R;
    return v;
}

uint32_t proStickEmulation(float lx, float ly, float rx, float ry) {
    constexpr float kThreshold = 0.5f;
    uint32_t v = 0;
    if (lx < -kThreshold) v |= WPAD_PRO_STICK_L_EMULATION_LEFT;
    if (lx > kThreshold) v |= WPAD_PRO_STICK_L_EMULATION_RIGHT;
    if (ly > kThreshold) v |= WPAD_PRO_STICK_L_EMULATION_UP;
    if (ly < -kThreshold) v |= WPAD_PRO_STICK_L_EMULATION_DOWN;
    if (rx < -kThreshold) v |= WPAD_PRO_STICK_R_EMULATION_LEFT;
    if (rx > kThreshold) v |= WPAD_PRO_STICK_R_EMULATION_RIGHT;
    if (ry > kThreshold) v |= WPAD_PRO_STICK_R_EMULATION_UP;
    if (ry < -kThreshold) v |= WPAD_PRO_STICK_R_EMULATION_DOWN;
    return v;
}

float wireToFloat(int16_t v) {
    return static_cast<float>(v) / static_cast<float>(WSU_STICK_MAX);
}

float absf(float v) { return v < 0 ? -v : v; }

// Maps normalized touch (0..4095) into the GamePad's raw touch-panel ADC
// span. Games run this through VPADGetTPCalibratedPoint, whose system
// calibration expects roughly this range; per-console calibration may be
// slightly off at the edges.
uint16_t touchToRaw(uint16_t normalized) {
    constexpr uint32_t kRawMin = 100;
    constexpr uint32_t kRawMax = 3996;
    if (normalized > WSU_TOUCH_MAX) normalized = WSU_TOUCH_MAX;
    return static_cast<uint16_t>(
        kRawMin + (static_cast<uint32_t>(normalized) * (kRawMax - kRawMin)) /
                      WSU_TOUCH_MAX);
}

// Previous merged hold per consumer, for trigger/release edges.
uint32_t gPrevVpadHold = 0;
uint32_t gPrevProHold[WSU_MAX_PLAYERS - 1] = {};

// WPAD channel (0..2) → player slot (1..3).
inline uint8_t chanToSlot(uint32_t chan) {
    return static_cast<uint8_t>(chan + 1);
}

void fillProStatus(KPADStatus *status, const WsuInputState &s,
                   uint32_t chan) {
    std::memset(status, 0, sizeof(*status));

    float lx = wireToFloat(s.lx);
    float ly = wireToFloat(s.ly);
    float rx = wireToFloat(s.rx);
    float ry = wireToFloat(s.ry);

    uint32_t hold =
        mapButtonsToPro(s.buttons) | proStickEmulation(lx, ly, rx, ry);
    uint32_t prev = gPrevProHold[chan];
    gPrevProHold[chan] = hold;

    status->extensionType = WPAD_EXT_PRO_CONTROLLER;
    status->error = 0; // KPAD_ERROR_OK
    status->format = WPAD_FMT_PRO_CONTROLLER;
    status->pro.hold = hold;
    status->pro.trigger = hold & ~prev;
    status->pro.release = prev & ~hold;
    status->pro.leftStick.x = lx;
    status->pro.leftStick.y = ly;
    status->pro.rightStick.x = rx;
    status->pro.rightStick.y = ry;
    status->pro.charging = FALSE;
    status->pro.wired = TRUE;
}

// Shared body of the KPADRead/KPADReadEx hooks. Returns the number of
// samples written, or UINT32_MAX to fall through to the real function.
uint32_t kpadInject(KPADChan chan, KPADStatus *data, uint32_t size,
                    KPADError *outError) {
    const int32_t c = static_cast<int32_t>(chan);
    if (c < 0 || c > 2 || data == nullptr || size == 0) {
        return UINT32_MAX;
    }
    WsuInputState s;
    if (!gRemoteInput.get(chanToSlot(static_cast<uint32_t>(c)), nowMs(),
                          &s)) {
        return UINT32_MAX;
    }
    fillProStatus(&data[0], s, static_cast<uint32_t>(c));
    if (outError != nullptr) {
        *outError = KPAD_ERROR_OK;
    }
    return 1;
}

} // namespace
} // namespace wsu

// ------------------------------------------------------------------
// VPADRead — GamePad (player slot 0)
// ------------------------------------------------------------------

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers,
              uint32_t count, VPADReadError *outError);

int32_t my_VPADRead(VPADChan chan, VPADStatus *buffers, uint32_t count,
                    VPADReadError *outError) {
    VPADReadError realError = VPAD_READ_SUCCESS;
    int32_t result = real_VPADRead(chan, buffers, count, &realError);
    if (outError != nullptr) *outError = realError;

    if (chan != VPAD_CHAN_0 || buffers == nullptr || count == 0) {
        return result;
    }

    WsuInputState s;
    if (!wsu::gRemoteInput.get(0, wsu::nowMs(), &s)) {
        // Remote inactive: track the real hold state so edges are right
        // the moment injection resumes.
        if (result > 0 && realError == VPAD_READ_SUCCESS) {
            wsu::gPrevVpadHold = buffers[0].hold;
        }
        return result;
    }

    // No physical GamePad (or no fresh sample): synthesize sample 0.
    bool synthesized = false;
    if (result <= 0 || realError != VPAD_READ_SUCCESS) {
        std::memset(&buffers[0], 0, sizeof(buffers[0]));
        buffers[0].battery = 6;
        synthesized = true;
    }

    VPADStatus &st = buffers[0];
    float lx = wsu::wireToFloat(s.lx);
    float ly = wsu::wireToFloat(s.ly);
    float rx = wsu::wireToFloat(s.rx);
    float ry = wsu::wireToFloat(s.ry);

    uint32_t remoteHold = wsu::mapButtonsToVpad(s.buttons) |
                          wsu::vpadStickEmulation(lx, ly, rx, ry);
    uint32_t hold = (synthesized ? 0 : st.hold) | remoteHold;
    st.trigger = hold & ~wsu::gPrevVpadHold;
    st.release = wsu::gPrevVpadHold & ~hold;
    st.hold = hold;
    wsu::gPrevVpadHold = hold;

    // Sticks: keep whichever input (local or remote) is deflected further.
    if (wsu::absf(lx) > wsu::absf(st.leftStick.x)) st.leftStick.x = lx;
    if (wsu::absf(ly) > wsu::absf(st.leftStick.y)) st.leftStick.y = ly;
    if (wsu::absf(rx) > wsu::absf(st.rightStick.x)) st.rightStick.x = rx;
    if (wsu::absf(ry) > wsu::absf(st.rightStick.y)) st.rightStick.y = ry;

    // Touch injection (mapped to raw panel coordinates).
    if (s.flags & WSU_INPUT_FLAG_TOUCHING) {
        VPADTouchData touch{};
        touch.x = wsu::touchToRaw(s.touchX);
        touch.y = wsu::touchToRaw(s.touchY);
        touch.touched = 1;
        touch.validity = VPAD_VALID;
        st.tpNormal = touch;
        st.tpFiltered1 = touch;
        st.tpFiltered2 = touch;
    }

    if (synthesized) {
        if (outError != nullptr) *outError = VPAD_READ_SUCCESS;
        return 1;
    }
    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

// ------------------------------------------------------------------
// KPADRead / KPADReadEx — Pro Controllers (player slots 1-3)
// ------------------------------------------------------------------

DECL_FUNCTION(uint32_t, KPADReadEx, KPADChan chan, KPADStatus *data,
              uint32_t size, KPADError *outError);

uint32_t my_KPADReadEx(KPADChan chan, KPADStatus *data, uint32_t size,
                       KPADError *outError) {
    uint32_t injected = wsu::kpadInject(chan, data, size, outError);
    if (injected != UINT32_MAX) return injected;
    return real_KPADReadEx(chan, data, size, outError);
}

WUPS_MUST_REPLACE(KPADReadEx, WUPS_LOADER_LIBRARY_PADSCORE, KPADReadEx);

DECL_FUNCTION(uint32_t, KPADRead, KPADChan chan, KPADStatus *data,
              uint32_t size);

uint32_t my_KPADRead(KPADChan chan, KPADStatus *data, uint32_t size) {
    uint32_t injected = wsu::kpadInject(chan, data, size, nullptr);
    if (injected != UINT32_MAX) return injected;
    return real_KPADRead(chan, data, size);
}

WUPS_MUST_REPLACE(KPADRead, WUPS_LOADER_LIBRARY_PADSCORE, KPADRead);

// ------------------------------------------------------------------
// WPADProbe — controller presence (player slots 1-3)
// ------------------------------------------------------------------

DECL_FUNCTION(int32_t, WPADProbe, WPADChan chan,
              WPADExtensionType *outExtensionType);

int32_t my_WPADProbe(WPADChan chan, WPADExtensionType *outExtensionType) {
    if (chan >= WPAD_CHAN_0 && chan <= WPAD_CHAN_2) {
        WsuInputState s;
        if (wsu::gRemoteInput.get(wsu::chanToSlot(chan), wsu::nowMs(), &s)) {
            if (outExtensionType != nullptr) {
                *outExtensionType = WPAD_EXT_PRO_CONTROLLER;
            }
            return 0; // success: controller present
        }
    }
    return real_WPADProbe(chan, outExtensionType);
}

WUPS_MUST_REPLACE(WPADProbe, WUPS_LOADER_LIBRARY_PADSCORE, WPADProbe);
