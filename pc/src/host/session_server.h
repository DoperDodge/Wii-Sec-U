// SessionServer — the host's fan-out / lobby (PLAN.md §4B.6).
//
// Accepts up to three remote clients over one UDP socket, assigns them
// player slots 1..3 (slot 0 is the host's own controller), relays the
// console's video/audio datagrams to every client, and feeds their INPUT
// packets into the InputHub. Slot assignment is server-authoritative:
// whatever slot a client claims in its INPUT packets is overwritten with
// the slot the server assigned to that endpoint.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/input_hub.h"
#include "net/udp_socket.h"
#include "wsu_protocol.h"

namespace wsu {

class SessionServer {
  public:
    struct ClientInfo {
        uint8_t slot = WSU_NO_SLOT;
        std::string name;
        std::string endpoint;
        int rttMs = -1;
    };

    SessionServer(InputHub &hub, uint16_t listenPort);
    ~SessionServer();

    bool start();
    void stop();

    // Port actually bound (differs from the requested one when 0 was
    // passed for tests).
    uint16_t port() const { return port_; }

    // Relays one already-serialized datagram (video/audio) to every
    // connected client. Called from the WiiULink stream thread.
    void broadcast(const uint8_t *buf, size_t len);

    // Stores the stream config; new clients get it on join and everyone
    // gets it when it changes.
    void setConfig(const WsuConfig &config);

    std::vector<ClientInfo> clients() const;

  private:
    struct Client {
        bool used = false;
        Endpoint ep;
        std::string name;
        uint32_t lastSeenMs = 0;
        int rttMs = -1;
        uint16_t txSeq = 0;
    };

    void loop();
    void handleHello(const Endpoint &from, const WsuHello &hello);
    void dropClient(size_t slotIdx, uint8_t reason);
    int findBySender(const Endpoint &from) const; // -1 if unknown

    InputHub &hub_;
    uint16_t requestedPort_;
    std::atomic<uint16_t> port_{0};
    UdpSocket socket_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    // Index i holds the client assigned player slot i+1.
    std::array<Client, WSU_MAX_PLAYERS - 1> clients_;
    WsuConfig config_{};
    bool haveConfig_ = false;
    uint16_t seq_ = 0;
};

} // namespace wsu
