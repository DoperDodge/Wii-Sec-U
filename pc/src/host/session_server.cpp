// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "host/session_server.h"

#include <cstring>

#include "util/log.h"
#include "util/time_util.h"

namespace wsu {

namespace {
constexpr const char *kTag = "session";
constexpr unsigned kRecvTimeoutMs = 100;
} // namespace

SessionServer::SessionServer(InputHub &hub, uint16_t listenPort)
    : hub_(hub), requestedPort_(listenPort) {}

SessionServer::~SessionServer() { stop(); }

bool SessionServer::start() {
    if (running_.load()) return true;
    if (!socket_.open(requestedPort_)) {
        logError(kTag, "failed to bind UDP port %u", requestedPort_);
        return false;
    }
    socket_.setRecvTimeout(kRecvTimeoutMs);
    port_.store(socket_.boundPort());
    running_.store(true);
    thread_ = std::thread(&SessionServer::loop, this);
    logInfo(kTag, "listening for clients on UDP %u", port_.load());
    return true;
}

void SessionServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t buf[WSU_HEADER_SIZE + 1];
        int n = wsu_pack_bye(buf, sizeof(buf), seq_++, WSU_BYE_SHUTDOWN);
        for (auto &c : clients_) {
            if (c.used && n > 0) socket_.sendTo(c.ep, buf, n);
            c.used = false;
        }
    }
    socket_.close();
}

void SessionServer::broadcast(const uint8_t *buf, size_t len) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &c : clients_) {
        if (c.used) socket_.sendTo(c.ep, buf, len);
    }
}

void SessionServer::setConfig(const WsuConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = !haveConfig_ ||
                   std::memcmp(&config, &config_, sizeof(config)) != 0;
    config_ = config;
    haveConfig_ = true;
    if (!changed) return;
    uint8_t buf[WSU_HEADER_SIZE + WSU_CONFIG_PAYLOAD_SIZE];
    int n = wsu_pack_config(buf, sizeof(buf), seq_++, &config_);
    for (auto &c : clients_) {
        if (c.used && n > 0) socket_.sendTo(c.ep, buf, n);
    }
}

std::vector<SessionServer::ClientInfo> SessionServer::clients() const {
    std::vector<ClientInfo> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < clients_.size(); i++) {
        if (!clients_[i].used) continue;
        ClientInfo info;
        info.slot = static_cast<uint8_t>(i + 1);
        info.name = clients_[i].name;
        info.endpoint = clients_[i].ep.toString();
        info.rttMs = clients_[i].rttMs;
        out.push_back(std::move(info));
    }
    return out;
}

int SessionServer::findBySender(const Endpoint &from) const {
    for (size_t i = 0; i < clients_.size(); i++) {
        if (clients_[i].used && clients_[i].ep == from) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SessionServer::handleHello(const Endpoint &from, const WsuHello &hello) {
    uint8_t buf[WSU_MAX_PACKET];
    std::lock_guard<std::mutex> lock(mutex_);

    int idx = findBySender(from);
    if (idx < 0) {
        for (size_t i = 0; i < clients_.size(); i++) {
            if (!clients_[i].used) {
                idx = static_cast<int>(i);
                break;
            }
        }
    }

    WsuHelloAck ack{};
    if (hello.role != WSU_ROLE_CLIENT) {
        ack.status = WSU_HELLO_REJECTED;
        ack.slot = WSU_NO_SLOT;
    } else if (idx < 0) {
        ack.status = WSU_HELLO_FULL;
        ack.slot = WSU_NO_SLOT;
    } else {
        Client &c = clients_[idx];
        bool isNew = !c.used;
        c.used = true;
        c.ep = from;
        c.name = hello.name;
        c.lastSeenMs = nowMs();
        ack.status = WSU_HELLO_OK;
        ack.slot = static_cast<uint8_t>(idx + 1);
        if (isNew) {
            logInfo(kTag, "client '%s' joined from %s as P%u", c.name.c_str(),
                    from.toString().c_str(), ack.slot + 1);
        }
    }

    int n = wsu_pack_hello_ack(buf, sizeof(buf), seq_++, &ack);
    if (n > 0) socket_.sendTo(from, buf, n);

    if (ack.status == WSU_HELLO_OK && haveConfig_) {
        n = wsu_pack_config(buf, sizeof(buf), seq_++, &config_);
        if (n > 0) socket_.sendTo(from, buf, n);
    }
}

void SessionServer::dropClient(size_t slotIdx, uint8_t reason) {
    Client &c = clients_[slotIdx];
    if (!c.used) return;
    logInfo(kTag, "client '%s' (P%u) left (reason %u)", c.name.c_str(),
            static_cast<unsigned>(slotIdx + 2), reason);
    c.used = false;
    hub_.clear(static_cast<uint8_t>(slotIdx + 1));
}

void SessionServer::loop() {
    uint8_t buf[WSU_MAX_PACKET];
    uint32_t lastSweep = 0;

    while (running_.load()) {
        uint32_t now = nowMs();
        if (ageMs(now, lastSweep) >= 1000) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < clients_.size(); i++) {
                if (clients_[i].used &&
                    ageMs(now, clients_[i].lastSeenMs) >
                        WSU_PEER_TIMEOUT_MS) {
                    dropClient(i, WSU_BYE_TIMEOUT);
                }
            }
            lastSweep = now;
        }

        size_t len = 0;
        Endpoint from;
        RecvResult r = socket_.recvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::Error) {
            if (running_.load()) sleepMs(50);
            continue;
        }
        if (r == RecvResult::Timeout) continue;

        WsuHeader hdr;
        if (wsu_parse_header(buf, len, &hdr) < 0) continue;
        const uint8_t *p = buf + WSU_HEADER_SIZE;
        size_t plen = len - WSU_HEADER_SIZE;
        now = nowMs();

        if (hdr.version != WSU_PROTO_VERSION) {
            if (hdr.type == WSU_PKT_HELLO) {
                WsuHelloAck ack{};
                ack.status = WSU_HELLO_VERSION_MISMATCH;
                ack.slot = WSU_NO_SLOT;
                int n = wsu_pack_hello_ack(buf, sizeof(buf), seq_++, &ack);
                if (n > 0) socket_.sendTo(from, buf, n);
            }
            continue;
        }

        switch (hdr.type) {
        case WSU_PKT_HELLO: {
            WsuHello hello;
            if (wsu_parse_hello(p, plen, &hello) >= 0) {
                handleHello(from, hello);
            }
            break;
        }
        case WSU_PKT_INPUT: {
            uint32_t ts;
            WsuInputState state;
            if (wsu_parse_input(p, plen, &ts, &state) < 0) break;
            std::lock_guard<std::mutex> lock(mutex_);
            int idx = findBySender(from);
            if (idx >= 0) {
                clients_[idx].lastSeenMs = now;
                hub_.set(static_cast<uint8_t>(idx + 1), state, now);
            }
            break;
        }
        case WSU_PKT_PING: {
            uint32_t ts;
            if (wsu_parse_timestamp(p, plen, &ts) < 0) break;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                int idx = findBySender(from);
                if (idx >= 0) clients_[idx].lastSeenMs = now;
            }
            uint8_t out[WSU_HEADER_SIZE + 4];
            int n = wsu_pack_pong(out, sizeof(out), seq_++, ts);
            if (n > 0) socket_.sendTo(from, out, n);
            break;
        }
        case WSU_PKT_BYE: {
            std::lock_guard<std::mutex> lock(mutex_);
            int idx = findBySender(from);
            if (idx >= 0) dropClient(idx, WSU_BYE_QUIT);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace wsu
