// WiiULink — the host PC's LAN connection to the console (PLAN.md §4B.4).
//
// The console runs two plugins with independent sockets:
//   - wsu-input  (WSU_CONSOLE_INPUT_PORT): receives INPUT_BUNDLE from us
//   - wsu-stream (WSU_CONSOLE_STREAM_PORT): sends VIDEO/AUDIO/CONFIG to us
//
// Each link handshakes separately: we send HELLO once per second until the
// plugin ACKs, then consider it up until WSU_PEER_TIMEOUT_MS of silence.
// The console address may be a concrete IP or the broadcast address for
// LAN discovery — the real console IP is learned from the ACK's source.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "net/udp_socket.h"
#include "wsu_protocol.h"

namespace wsu {

class WiiULink {
  public:
    struct Callbacks {
        // Raw, still-serialized datagram plus its parsed header — the host
        // relays the exact bytes to clients without re-slicing.
        std::function<void(const uint8_t *datagram, size_t len,
                           const WsuVideoHeader &v, const uint8_t *payload)>
            onVideo;
        std::function<void(const uint8_t *datagram, size_t len,
                           const WsuAudioHeader &a, const uint8_t *payload)>
            onAudio;
        std::function<void(const WsuConfig &config)> onConfig;
    };

    // `consoleAddr` needs no port; input/stream ports are taken from
    // `inputPort`/`streamPort` (overridable for tests).
    WiiULink(Endpoint consoleAddr, uint16_t inputPort, uint16_t streamPort,
             Callbacks callbacks);
    ~WiiULink();

    bool start();
    void stop();

    // Sends a ready-packed INPUT_BUNDLE datagram to the input plugin.
    // Callable from any thread once start() returned true.
    void sendInputBundle(const uint8_t *buf, size_t len);

    bool inputLinkUp() const { return inputUp_.load(); }
    bool streamLinkUp() const { return streamUp_.load(); }
    // Smoothed RTT to the input plugin in ms, or -1 if unknown.
    int rttMs() const { return rttMs_.load(); }

  private:
    void inputLoop();
    void streamLoop();

    Endpoint consoleBase_;
    uint16_t consoleInputPort_;
    uint16_t consoleStreamPort_;
    Callbacks callbacks_;

    UdpSocket inputSocket_;
    UdpSocket streamSocket_;
    std::mutex inputEpMutex_;
    Endpoint inputEp_;  // resolved console endpoint for input
    std::atomic<bool> running_{false};
    std::atomic<bool> inputUp_{false};
    std::atomic<bool> streamUp_{false};
    std::atomic<int> rttMs_{-1};
    std::thread inputThread_;
    std::thread streamThread_;
    uint16_t seq_ = 0;
};

} // namespace wsu
