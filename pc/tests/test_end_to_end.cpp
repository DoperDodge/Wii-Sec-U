// End-to-end integration test over loopback UDP:
//
//   ConsoleSim (fake Wii U) ⇄ HostApp ⇄ ClientApp
//
// Asserts the full PLAN.md data flow on one machine:
//   video:  sim → host (assembled locally) → relayed → client (assembled)
//   input:  host P1 (scripted) + client P2 → INPUT_BUNDLE → sim
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <atomic>
#include <memory>
#include <thread>

#include "client/client_app.h"
#include "host/host_app.h"
#include "sim/console_sim.h"
#include "test_common.h"
#include "util/log.h"
#include "util/time_util.h"

using namespace wsu;

namespace {

// Waits up to `timeoutMs` for `pred` to become true.
template <typename Pred> bool waitFor(Pred pred, unsigned timeoutMs) {
    uint32_t start = nowMs();
    while (ageMs(nowMs(), start) < timeoutMs) {
        if (pred()) return true;
        sleepMs(20);
    }
    return pred();
}

} // namespace

int main() {
    setLogLevel(LogLevel::Warn); // keep CI output quiet

    // --- Fake console on ephemeral loopback ports. ---
    ConsoleSimOptions simOpts;
    simOpts.inputPort = 0;
    simOpts.streamPort = 0;
    simOpts.width = 96;
    simOpts.height = 54;
    simOpts.fps = 30;
    ConsoleSim sim(simOpts);
    CHECK(sim.start());

    // --- Host pointed at the sim, listening on an ephemeral port. ---
    HostOptions hostOpts;
    hostOpts.consoleAddr = Endpoint::loopback(1);
    hostOpts.consoleInputPort = sim.inputPort();
    hostOpts.consoleStreamPort = sim.streamPort();
    hostOpts.listenPort = 0;
    hostOpts.printStats = false;
    auto hostSink = std::make_shared<StatsSink>();
    HostApp host(hostOpts, makeSyntheticBackend(true), hostSink);

    std::atomic<bool> stopHost{false};
    std::thread hostThread([&] { CHECK(host.run(stopHost)); });

    // Both console links must come up.
    CHECK(waitFor([&] { return host.link().inputLinkUp(); }, 5000));
    CHECK(waitFor([&] { return host.link().streamLinkUp(); }, 5000));
    CHECK(waitFor([&] { return host.sessions().port() != 0; }, 1000));

    // --- Remote client joins the host. ---
    ClientOptions clientOpts;
    clientOpts.hostAddr = Endpoint::loopback(host.sessions().port());
    clientOpts.playerName = "luigi";
    clientOpts.printStats = false;
    auto clientSink = std::make_shared<StatsSink>();
    ClientApp client(clientOpts, makeSyntheticBackend(true), clientSink);

    std::atomic<bool> stopClient{false};
    std::thread clientThread([&] { CHECK(client.run(stopClient)); });

    CHECK(waitFor([&] { return client.connected(); }, 5000));
    CHECK_EQ(client.slot(), 1); // first remote player → slot 1 (P2)

    // --- Video flows: sim → host sink, and sim → host → client sink. ---
    CHECK(waitFor([&] { return hostSink->frames() >= 10; }, 10000));
    CHECK(waitFor([&] { return clientSink->frames() >= 10; }, 10000));

    // Both ends learned the stream config.
    CHECK(waitFor([&] { return hostSink->haveConfig(); }, 2000));
    CHECK(waitFor([&] { return clientSink->haveConfig(); }, 2000));
    CHECK_EQ(hostSink->width(), 96);
    CHECK_EQ(clientSink->width(), 96);
    CHECK_EQ(clientSink->codec(), WSU_CODEC_RAWRGB);

    // A complete RAWRGB frame has exactly w*h*3 bytes — slicing and
    // reassembly preserved sizes through two hops.
    CHECK(waitFor(
        [&] {
            return hostSink->videoBytes() % (96 * 54 * 3) == 0 &&
                   hostSink->videoBytes() > 0;
        },
        2000));

    // --- Input flows: both controllers reach the fake console. ---
    CHECK(waitFor([&] { return sim.bundlesReceived() >= 30; }, 5000));

    // P1 (host local) and P2 (client) both present with the scripted
    // pattern: a stick pushed to an extreme.
    CHECK(waitFor(
        [&] {
            WsuInputState s;
            return sim.lastInput(0, s) &&
                   (s.lx == WSU_STICK_MAX || s.lx == -WSU_STICK_MAX ||
                    s.ly == WSU_STICK_MAX || s.ly == -WSU_STICK_MAX);
        },
        5000));
    CHECK(waitFor(
        [&] {
            WsuInputState s;
            return sim.lastInput(1, s) &&
                   (s.lx == WSU_STICK_MAX || s.lx == -WSU_STICK_MAX ||
                    s.ly == WSU_STICK_MAX || s.ly == -WSU_STICK_MAX);
        },
        5000));

    // Slots 2/3 never joined; the sim must not have seen them.
    {
        WsuInputState s;
        CHECK(!sim.lastInput(2, s));
        CHECK(!sim.lastInput(3, s));
    }

    // The lobby lists exactly one client with the right name.
    {
        auto clients = host.sessions().clients();
        CHECK_EQ(clients.size(), 1u);
        if (!clients.empty()) {
            CHECK_EQ(clients[0].slot, 1);
            CHECK(clients[0].name == "luigi");
        }
    }

    // --- Orderly shutdown. ---
    stopClient.store(true);
    clientThread.join();
    stopHost.store(true);
    hostThread.join();
    sim.stop();

    return testSummary("test_end_to_end");
}
