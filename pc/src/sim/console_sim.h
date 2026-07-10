// ConsoleSim — a PC stand-in for the Wii U plugins.
//
// Speaks the exact same protocol as wsu-input/wsu-stream: ACKs the host's
// HELLOs, streams a WSU_CODEC_RAWRGB test pattern at the configured rate,
// and records every INPUT_BUNDLE it receives. This lets the entire PC
// stack (host relay, fan-out, client) be exercised end-to-end on one
// machine with no console — used by `wsu console-sim` and the e2e test.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "net/udp_socket.h"
#include "wsu_protocol.h"

namespace wsu {

struct ConsoleSimOptions {
    uint16_t inputPort = WSU_CONSOLE_INPUT_PORT;
    uint16_t streamPort = WSU_CONSOLE_STREAM_PORT;
    uint16_t width = 160;
    uint16_t height = 90;
    uint8_t fps = 15;
    bool logInput = false; // print a line whenever P-state changes
};

class ConsoleSim {
  public:
    explicit ConsoleSim(ConsoleSimOptions options);
    ~ConsoleSim();

    bool start();
    void stop();

    // Ports actually bound (for tests using ephemeral ports).
    uint16_t inputPort() const { return inputPort_.load(); }
    uint16_t streamPort() const { return streamPort_.load(); }

    // Test introspection.
    uint64_t bundlesReceived() const { return bundles_.load(); }
    uint32_t framesSent() const { return framesSent_.load(); }
    bool hostConnected() const { return hostKnown_.load(); }
    // Latest received state for a slot; false if that slot never appeared.
    bool lastInput(uint8_t slot, WsuInputState &out) const;

  private:
    void inputLoop();
    void streamLoop();
    void buildFrame(std::vector<uint8_t> &rgb, uint32_t frameId) const;

    ConsoleSimOptions options_;
    UdpSocket inputSocket_;
    UdpSocket streamSocket_;
    std::atomic<uint16_t> inputPort_{0};
    std::atomic<uint16_t> streamPort_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> hostKnown_{false};
    std::atomic<uint64_t> bundles_{0};
    std::atomic<uint32_t> framesSent_{0};
    std::thread inputThread_;
    std::thread streamThread_;

    mutable std::mutex inputMutex_;
    std::array<WsuInputState, WSU_MAX_PLAYERS> lastInput_{};
    std::array<bool, WSU_MAX_PLAYERS> haveInput_{};
};

} // namespace wsu
