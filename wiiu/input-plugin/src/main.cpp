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
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>

#include <wups.h>

#include "remote_input.h"
#include "wsu_protocol.h"
#include "wsu_wiiu.h"

WUPS_PLUGIN_NAME("wsu-input");
WUPS_PLUGIN_DESCRIPTION("Wii-Sec-U: network controller injection (P1-P4)");
WUPS_PLUGIN_VERSION("v0.1.0");
WUPS_PLUGIN_AUTHOR("Wii-Sec-U contributors");
WUPS_PLUGIN_LICENSE("GPLv3");

// WUPS_PLUGIN_NAME already pulls in the wut malloc/sockets/newlib/stdcpp/
// thread hooks; only the devoptab (fs access for the config file) is extra.
WUPS_USE_WUT_DEVOPTAB();

namespace wsu {

RemoteInput gRemoteInput;

namespace {

constexpr uint32_t kNetStackSize = 64 * 1024;
constexpr int32_t kNetPriority = 20; // slightly below the app default 16

std::atomic<bool> gRunning{false};
int gPort = WSU_CONSOLE_INPUT_PORT;
uint16_t gTxSeq = 0;

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

    while (gRunning.load()) {
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
            }
            break;
        }
        case WSU_PKT_BYE:
            WSU_LOG("input: host disconnected");
            gRemoteInput.deactivateAll();
            break;
        default:
            break;
        }
    }

    close(sock);
    return 0;
}

PluginThread gNetThread;

} // namespace
} // namespace wsu

INITIALIZE_PLUGIN() {
    wsu::logInit();
    wsu::gRemoteInput.init();

    wsu::ConfigFile config("fs:/vol/external01/wiiu/wsu-input.cfg");
    wsu::gPort = config.getInt("port", WSU_CONSOLE_INPUT_PORT);
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
