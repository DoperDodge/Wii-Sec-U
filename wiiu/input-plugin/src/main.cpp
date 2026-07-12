// wsu-input — Aroma (WUPS) plugin that receives up to four controllers
// from the host PC and injects them into the running game (PLAN.md §4A.5).
//
//   host PC ── INPUT_BUNDLE (UDP :4404) ──► this plugin ──► VPAD/KPAD hooks
//
// The network thread answers the host's HELLO/PING and stores the latest
// full state per player slot; the read hooks in injection.cpp overlay that
// state onto what the game reads. Injection self-heals: a slot with no
// update for WSU_INPUT_TIMEOUT_MS drops back to "not present".
//
// Settings live in the Aroma plugin config menu (open it in-game with
// L + D-Pad Down + SELECT): a master switch plus per-player toggles, all
// applied immediately and persisted via the WUPS storage API.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>
#include <cstdio>
#include <string_view>

#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>
#include <wups/storage.h>

#include "remote_input.h"
#include "wsu_protocol.h"
#include "wsu_wiiu.h"

WUPS_PLUGIN_NAME("wsu-input");
WUPS_PLUGIN_DESCRIPTION("Wii-Sec-U: network controller injection (P1-P4)");
WUPS_PLUGIN_VERSION("v0.2.0");
WUPS_PLUGIN_AUTHOR("Wii-Sec-U contributors");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("wsu_input");

namespace wsu {

RemoteInput gRemoteInput;

namespace {

constexpr uint32_t kNetStackSize = 64 * 1024;
constexpr int32_t kNetPriority = 20; // slightly below the app default 16

std::atomic<bool> gRunning{false};
std::atomic<bool> gMasterEnabled{true};
std::atomic<bool> gSlotEnabled[WSU_MAX_PLAYERS] = {true, true, true, true};
std::atomic<bool> gHostConnected{false};
int gPort = WSU_CONSOLE_INPUT_PORT;
uint16_t gTxSeq = 0;

const char *kSlotKeys[WSU_MAX_PLAYERS] = {"slot1", "slot2", "slot3",
                                          "slot4"};

void sendTo(int sock, const sockaddr_in &to, const uint8_t *buf, int len) {
    if (len <= 0) return;
    sendto(sock, buf, static_cast<size_t>(len), 0,
           reinterpret_cast<const sockaddr *>(&to), sizeof(to));
}

int netThread(int, const char **) {
    int sock = openUdpSocket(static_cast<uint16_t>(gPort));
    if (sock < 0) {
        WSU_LOG("input: failed to open UDP %d", gPort);
        return -1;
    }
    WSU_LOG("input: listening on UDP %d", gPort);

    uint8_t buf[WSU_MAX_PACKET];
    sockaddr_in from;
    socklen_t fromLen;
    uint32_t lastHostSeenMs = 0;

    while (gRunning.load()) {
        if (gHostConnected.load() && lastHostSeenMs != 0 &&
            ageMs(nowMs(), lastHostSeenMs) > WSU_PEER_TIMEOUT_MS) {
            gHostConnected.store(false);
        }

        fromLen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (len <= 0) {
            sleepMs(2); // non-blocking socket; ~2ms poll keeps input fresh
            continue;
        }

        WsuHeader hdr;
        if (wsu_parse_header(buf, static_cast<size_t>(len), &hdr) < 0) {
            continue;
        }
        const uint8_t *p = buf + WSU_HEADER_SIZE;
        size_t plen = static_cast<size_t>(len) - WSU_HEADER_SIZE;

        switch (hdr.type) {
        case WSU_PKT_HELLO: {
            WsuHelloAck ack{};
            ack.status = hdr.version == WSU_PROTO_VERSION
                             ? WSU_HELLO_OK
                             : WSU_HELLO_VERSION_MISMATCH;
            ack.slot = WSU_NO_SLOT;
            uint8_t out[WSU_HEADER_SIZE + WSU_HELLO_ACK_PAYLOAD_SIZE];
            int n = wsu_pack_hello_ack(out, sizeof(out), gTxSeq++, &ack);
            sendTo(sock, from, out, n);
            if (ack.status == WSU_HELLO_OK) {
                WSU_LOG("input: host connected");
                gHostConnected.store(true);
                lastHostSeenMs = nowMs();
            }
            break;
        }
        case WSU_PKT_INPUT_BUNDLE: {
            uint32_t ts;
            WsuInputState states[WSU_MAX_PLAYERS];
            uint8_t count = 0;
            if (wsu_parse_input_bundle(p, plen, &ts, states, &count) < 0) {
                break;
            }
            uint32_t now = nowMs();
            lastHostSeenMs = now;
            gHostConnected.store(true);
            for (uint8_t i = 0; i < count; i++) {
                gRemoteInput.update(states[i], now);
            }
            break;
        }
        case WSU_PKT_PING: {
            uint32_t ts;
            if (wsu_parse_timestamp(p, plen, &ts) == 4) {
                uint8_t out[WSU_HEADER_SIZE + 4];
                int n = wsu_pack_pong(out, sizeof(out), gTxSeq++, ts);
                sendTo(sock, from, out, n);
                lastHostSeenMs = nowMs();
            }
            break;
        }
        case WSU_PKT_BYE:
            WSU_LOG("input: host disconnected");
            gRemoteInput.deactivateAll();
            gHostConnected.store(false);
            break;
        default:
            break;
        }
    }

    close(sock);
    return 0;
}

PluginThread gNetThread;

// ---- config menu ----

void masterChanged(ConfigItemBoolean *, bool newValue) {
    gMasterEnabled.store(newValue);
    WUPSStorageAPI::Store("enabled", newValue);
}

void slotChanged(ConfigItemBoolean *item, bool newValue) {
    for (int i = 0; i < WSU_MAX_PLAYERS; i++) {
        if (std::string_view(kSlotKeys[i]) == item->identifier) {
            gSlotEnabled[i].store(newValue);
            WUPSStorageAPI::Store(item->identifier, newValue);
            return;
        }
    }
}

WUPSConfigAPICallbackStatus configMenuOpened(
    WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);

        static char status[64];
        std::snprintf(status, sizeof(status), "Host PC: %s",
                      gHostConnected.load() ? "connected"
                                            : "not connected");
        root.add(WUPSConfigItemStub::Create(status));

        root.add(WUPSConfigItemBoolean::Create(
            "enabled", "Enable network controllers", true,
            gMasterEnabled.load(), masterChanged));

        static const char *labels[WSU_MAX_PLAYERS] = {
            "Player 1 (GamePad)", "Player 2 (Pro Controller)",
            "Player 3 (Pro Controller)", "Player 4 (Pro Controller)"};
        for (int i = 0; i < WSU_MAX_PLAYERS; i++) {
            root.add(WUPSConfigItemBoolean::Create(
                kSlotKeys[i], labels[i], true, gSlotEnabled[i].load(),
                slotChanged));
        }

        static char portText[40];
        std::snprintf(portText, sizeof(portText), "UDP port: %d (fixed)",
                      gPort);
        root.add(WUPSConfigItemStub::Create(portText));
    } catch (std::exception &e) {
        WSU_LOG("input: config menu failed: %s", e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void configMenuClosed() {
    WUPSStorageAPI::SaveStorage();
}

void loadSettings() {
    bool enabled = true;
    WUPSStorageAPI::GetOrStoreDefault("enabled", enabled, true);
    gMasterEnabled.store(enabled);
    for (int i = 0; i < WSU_MAX_PLAYERS; i++) {
        bool slotOn = true;
        WUPSStorageAPI::GetOrStoreDefault(kSlotKeys[i], slotOn, true);
        gSlotEnabled[i].store(slotOn);
    }
    WUPSStorageAPI::SaveStorage();
}

} // namespace

// Queried by the injection hooks (injection.cpp).
bool slotInjectionEnabled(uint8_t slot) {
    if (!gMasterEnabled.load()) return false;
    if (slot >= WSU_MAX_PLAYERS) return false;
    return gSlotEnabled[slot].load();
}

} // namespace wsu

INITIALIZE_PLUGIN() {
    wsu::logInit();
    wsu::gRemoteInput.init();

    WUPSConfigAPIOptionsV1 configOptions = {.name = "Wii-Sec-U input"};
    if (WUPSConfigAPI_Init(configOptions, wsu::configMenuOpened,
                           wsu::configMenuClosed) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        WSU_LOG("input: failed to init config API");
    }
    wsu::loadSettings();
    WSU_LOG("wsu-input initialized (port %d)", wsu::gPort);
}

ON_APPLICATION_START() {
    wsu::logInit();
    wsu::gRemoteInput.init();
    wsu::gRunning.store(true);
    if (!wsu::gNetThread.start(wsu::netThread, wsu::kNetStackSize,
                               wsu::kNetPriority,
                               OS_THREAD_ATTRIB_AFFINITY_CPU2,
                               "wsu-input-net")) {
        WSU_LOG("input: failed to start network thread");
        wsu::gRunning.store(false);
    }
}

ON_APPLICATION_ENDS() {
    wsu::gRunning.store(false);
    wsu::gNetThread.join();
    wsu::gRemoteInput.deactivateAll();
}
