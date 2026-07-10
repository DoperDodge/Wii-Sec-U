// wsu — unified PC app for Wii-Sec-U (PLAN.md §4B).
//
//   wsu host        run on the PC that shares a LAN with the Wii U
//   wsu client      join a host over the network as player 2-4
//   wsu console-sim pretend to be a Wii U (development without hardware)
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "client/client_app.h"
#include "host/host_app.h"
#include "sim/console_sim.h"
#include "util/log.h"
#include "util/time_util.h"

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop.store(true); }

void installSignalHandlers() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
}

void printUsage() {
    std::fprintf(stderr, R"(wsu — Wii U remote co-op ("Parsec for Wii U")

Usage:
  wsu host [options]           Run the host (PC on the same LAN as the Wii U)
    --console IP               Wii U IP address (default: LAN broadcast)
    --listen-port N            UDP port for remote players (default %u)
    --input synthetic|scripted|sdl|none
                               Local P1 controller source (default: sdl if
                               built with SDL, else synthetic)
    --console-input-port N     Override console input port (default %u)
    --console-stream-port N    Override console stream port (default %u)

  wsu client [options]         Join a host as a remote player
    --host IP[:port]           Host address (required; default port %u)
    --name NAME                Player name shown in the lobby
    --input synthetic|scripted|sdl|none

  wsu console-sim [options]    Simulate a Wii U for development
    --input-port N             (default %u)   --stream-port N (default %u)
    --size WxH                 Test pattern size (default 160x90)
    --fps N                    Test pattern rate (default 15)
    --log-input                Print received controller state changes

Common:
    --verbose                  Debug logging
)",
                 WSU_HOST_PORT, WSU_CONSOLE_INPUT_PORT,
                 WSU_CONSOLE_STREAM_PORT, WSU_HOST_PORT,
                 WSU_CONSOLE_INPUT_PORT, WSU_CONSOLE_STREAM_PORT);
}

// Returns the value following `flag`, or nullptr.
const char *argValue(int argc, char **argv, const char *flag) {
    for (int i = 2; i < argc - 1; i++) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

bool argPresent(int argc, char **argv, const char *flag) {
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

std::unique_ptr<wsu::InputBackend> makeInput(const char *choice) {
    using namespace wsu;
    std::string c = choice != nullptr ? choice : "";
    if (c == "none") return nullptr;
    if (c == "synthetic") return makeSyntheticBackend(false);
    if (c == "scripted") return makeSyntheticBackend(true);
    if (c == "sdl" || c.empty()) {
        auto sdl = makeSdlBackend();
        if (sdl != nullptr) return sdl;
        if (c == "sdl") {
            logError("main", "built without SDL support (-DWSU_WITH_SDL=ON)");
            return nullptr;
        }
        logWarn("main", "no SDL support built in; using neutral synthetic "
                        "input for P1");
        return makeSyntheticBackend(false);
    }
    logError("main", "unknown input backend '%s'", c.c_str());
    return nullptr;
}

int runHost(int argc, char **argv) {
    using namespace wsu;
    HostOptions opts;
    // Only the address matters here; the per-plugin ports are separate
    // options, so parse with a dummy port to satisfy validation.
    const char *console = argValue(argc, argv, "--console");
    opts.consoleAddr = console != nullptr ? Endpoint::parse(console, 1)
                                          : Endpoint::broadcast(1);
    if (!opts.consoleAddr.valid()) {
        logError("main", "bad --console address '%s'",
                 console != nullptr ? console : "");
        return 2;
    }

    if (const char *v = argValue(argc, argv, "--listen-port")) {
        opts.listenPort = static_cast<uint16_t>(std::atoi(v));
    }
    if (const char *v = argValue(argc, argv, "--console-input-port")) {
        opts.consoleInputPort = static_cast<uint16_t>(std::atoi(v));
    }
    if (const char *v = argValue(argc, argv, "--console-stream-port")) {
        opts.consoleStreamPort = static_cast<uint16_t>(std::atoi(v));
    }

    auto input = makeInput(argValue(argc, argv, "--input"));
    auto sink = std::make_shared<StatsSink>();
    HostApp app(opts, std::move(input), sink);
    return app.run(gStop) ? 0 : 1;
}

int runClient(int argc, char **argv) {
    using namespace wsu;
    const char *host = argValue(argc, argv, "--host");
    if (host == nullptr) {
        logError("main", "--host is required for client mode");
        return 2;
    }
    ClientOptions opts;
    opts.hostAddr = Endpoint::parse(host, WSU_HOST_PORT);
    if (!opts.hostAddr.valid()) {
        logError("main", "bad --host address '%s'", host);
        return 2;
    }
    if (const char *v = argValue(argc, argv, "--name")) {
        opts.playerName = v;
    }
    auto input = makeInput(argValue(argc, argv, "--input"));
    auto sink = std::make_shared<StatsSink>();
    ClientApp app(opts, std::move(input), sink);
    return app.run(gStop) ? 0 : 1;
}

int runConsoleSim(int argc, char **argv) {
    using namespace wsu;
    ConsoleSimOptions opts;
    if (const char *v = argValue(argc, argv, "--input-port")) {
        opts.inputPort = static_cast<uint16_t>(std::atoi(v));
    }
    if (const char *v = argValue(argc, argv, "--stream-port")) {
        opts.streamPort = static_cast<uint16_t>(std::atoi(v));
    }
    if (const char *v = argValue(argc, argv, "--size")) {
        unsigned w = 0, h = 0;
        if (std::sscanf(v, "%ux%u", &w, &h) == 2 && w > 0 && h > 0 &&
            w <= 1920 && h <= 1080) {
            opts.width = static_cast<uint16_t>(w);
            opts.height = static_cast<uint16_t>(h);
        }
    }
    if (const char *v = argValue(argc, argv, "--fps")) {
        int fps = std::atoi(v);
        if (fps > 0 && fps <= 60) opts.fps = static_cast<uint8_t>(fps);
    }
    opts.logInput = argPresent(argc, argv, "--log-input");

    ConsoleSim sim(opts);
    if (!sim.start()) return 1;
    while (!gStop.load()) sleepMs(100);
    sim.stop();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    if (argPresent(argc, argv, "--verbose")) {
        wsu::setLogLevel(wsu::LogLevel::Debug);
    }
    installSignalHandlers();

    std::string mode = argv[1];
    if (mode == "host") return runHost(argc, argv);
    if (mode == "client") return runClient(argc, argv);
    if (mode == "console-sim") return runConsoleSim(argc, argv);
    if (mode == "--help" || mode == "-h" || mode == "help") {
        printUsage();
        return 0;
    }
    std::fprintf(stderr, "unknown mode '%s'\n\n", mode.c_str());
    printUsage();
    return 2;
}
