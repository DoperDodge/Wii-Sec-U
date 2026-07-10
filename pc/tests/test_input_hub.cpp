// InputHub merge/timeout tests.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "core/input_hub.h"
#include "test_common.h"

using namespace wsu;

int main() {
    InputHub hub;
    uint8_t buf[WSU_MAX_PACKET];
    const uint32_t t0 = 100000;

    // Empty hub produces no bundle.
    CHECK_EQ(hub.packBundle(buf, sizeof(buf), 0, t0), 0);

    // One active slot.
    WsuInputState p1;
    wsu_input_state_init(&p1, 0);
    p1.buttons = WSU_BTN_A;
    p1.lx = 1000;
    hub.set(0, p1, t0);
    CHECK(hub.isActive(0, t0));

    int n = hub.packBundle(buf, sizeof(buf), 1, t0 + 10);
    CHECK(n > 0);
    {
        WsuHeader hdr;
        CHECK(wsu_parse_header(buf, n, &hdr) > 0);
        CHECK_EQ(hdr.type, WSU_PKT_INPUT_BUNDLE);
        uint32_t ts;
        WsuInputState out[WSU_MAX_PLAYERS];
        uint8_t count;
        CHECK(wsu_parse_input_bundle(buf + WSU_HEADER_SIZE,
                                     n - WSU_HEADER_SIZE, &ts, out,
                                     &count) > 0);
        CHECK_EQ(count, 1);
        CHECK_EQ(out[0].slot, 0);
        CHECK_EQ(out[0].buttons, static_cast<uint32_t>(WSU_BTN_A));
        CHECK_EQ(out[0].lx, 1000);
    }

    // The hub enforces the slot it was given, not the one in the state.
    WsuInputState imposter;
    wsu_input_state_init(&imposter, 0); // claims slot 0
    imposter.buttons = WSU_BTN_B;
    hub.set(2, imposter, t0);
    {
        n = hub.packBundle(buf, sizeof(buf), 2, t0 + 10);
        uint32_t ts;
        WsuInputState out[WSU_MAX_PLAYERS];
        uint8_t count;
        wsu_parse_input_bundle(buf + WSU_HEADER_SIZE, n - WSU_HEADER_SIZE,
                               &ts, out, &count);
        CHECK_EQ(count, 2);
        CHECK_EQ(out[1].slot, 2);
        CHECK_EQ(out[1].buttons, static_cast<uint32_t>(WSU_BTN_B));
    }

    // Slots expire after WSU_INPUT_TIMEOUT_MS.
    CHECK(hub.isActive(0, t0 + WSU_INPUT_TIMEOUT_MS - 1));
    CHECK(!hub.isActive(0, t0 + WSU_INPUT_TIMEOUT_MS + 1));
    CHECK_EQ(hub.packBundle(buf, sizeof(buf), 3,
                            t0 + WSU_INPUT_TIMEOUT_MS + 1),
             0);

    // clear() deactivates immediately.
    hub.set(1, p1, t0 + 500);
    CHECK(hub.isActive(1, t0 + 500));
    hub.clear(1);
    CHECK(!hub.isActive(1, t0 + 500));

    // Out-of-range slots are ignored, not fatal.
    hub.set(WSU_MAX_PLAYERS, p1, t0);
    hub.clear(WSU_MAX_PLAYERS);
    CHECK(!hub.isActive(WSU_MAX_PLAYERS, t0));

    return testSummary("test_input_hub");
}
