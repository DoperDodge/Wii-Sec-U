// ClientApp — the remote player role (PLAN.md §4B, "Client role").
//
// Connects to a host by address, claims a player slot via HELLO, then:
//   in:  VIDEO slices → FrameAssembler → VideoSink, AUDIO → VideoSink
//   out: full controller state at the input rate, PING once per second
// Reconnects automatically if the host goes silent.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "core/frame_assembler.h"
#include "input/input_backend.h"
#include "net/udp_socket.h"
#include "video/video_sink.h"

namespace wsu {

struct ClientOptions {
    Endpoint hostAddr;
    std::string playerName = "player";
    unsigned inputRateHz = WSU_INPUT_RATE_HZ;
    bool printStats = true;
};

class ClientApp {
  public:
    ClientApp(ClientOptions options, std::unique_ptr<InputBackend> input,
              std::shared_ptr<VideoSink> sink);

    // Runs until `stop` becomes true. Returns false on startup failure.
    bool run(const std::atomic<bool> &stop);

    bool connected() const { return connected_.load(); }
    // Assigned player slot (1..3), or WSU_NO_SLOT before the first ACK.
    uint8_t slot() const { return slot_.load(); }
    int rttMs() const { return rttMs_.load(); }

  private:
    ClientOptions options_;
    std::unique_ptr<InputBackend> input_;
    std::shared_ptr<VideoSink> sink_;
    std::atomic<bool> connected_{false};
    std::atomic<uint8_t> slot_{WSU_NO_SLOT};
    std::atomic<int> rttMs_{-1};
};

} // namespace wsu
