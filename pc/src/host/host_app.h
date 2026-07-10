// HostApp — composition root for the host role (PLAN.md §4B, "Host role").
//
// Wires together: WiiULink (console on the LAN) + SessionServer (remote
// clients) + InputHub (P1..P4 merge) + a local input backend (P1) + a
// local video sink. Video/audio datagrams from the console are relayed
// byte-for-byte to every client and simultaneously fed to the local sink.
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <atomic>
#include <memory>

#include "core/frame_assembler.h"
#include "core/input_hub.h"
#include "host/session_server.h"
#include "host/wiiu_link.h"
#include "input/input_backend.h"
#include "video/video_sink.h"

namespace wsu {

struct HostOptions {
    Endpoint consoleAddr;             // may be Endpoint::broadcast(...)
    uint16_t consoleInputPort = WSU_CONSOLE_INPUT_PORT;
    uint16_t consoleStreamPort = WSU_CONSOLE_STREAM_PORT;
    uint16_t listenPort = WSU_HOST_PORT;
    unsigned inputRateHz = WSU_INPUT_RATE_HZ;
    bool printStats = true;
};

class HostApp {
  public:
    HostApp(HostOptions options, std::unique_ptr<InputBackend> localInput,
            std::shared_ptr<VideoSink> localSink);
    ~HostApp();

    // Runs until `stop` becomes true. Returns false on startup failure.
    bool run(const std::atomic<bool> &stop);

    // Introspection for tests/UI.
    const StatsSink *statsSink() const;
    SessionServer &sessions() { return server_; }
    WiiULink &link() { return link_; }
    InputHub &hub() { return hub_; }

  private:
    void onVideo(const uint8_t *datagram, size_t len,
                 const WsuVideoHeader &v, const uint8_t *payload);
    void onAudio(const uint8_t *datagram, size_t len,
                 const WsuAudioHeader &a, const uint8_t *payload);
    void onConfig(const WsuConfig &config);

    HostOptions options_;
    std::unique_ptr<InputBackend> localInput_;
    std::shared_ptr<VideoSink> localSink_;
    InputHub hub_;
    SessionServer server_;
    WiiULink link_;
    FrameAssembler assembler_;
};

} // namespace wsu
