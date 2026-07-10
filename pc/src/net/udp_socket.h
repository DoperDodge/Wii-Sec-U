// Cross-platform (WinSock2 / BSD sockets) blocking UDP socket with a
// receive timeout, plus a small IPv4 endpoint value type.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdint>
#include <string>

namespace wsu {

// IPv4 address + port. `addr` is in host byte order.
struct Endpoint {
    uint32_t addr = 0;
    uint16_t port = 0;

    bool valid() const { return port != 0; }
    bool operator==(const Endpoint &o) const {
        return addr == o.addr && port == o.port;
    }
    bool operator!=(const Endpoint &o) const { return !(*this == o); }

    std::string toString() const;

    // Parses "a.b.c.d" or "a.b.c.d:port"; `defaultPort` fills in a missing
    // port. Returns an invalid endpoint on parse failure.
    static Endpoint parse(const std::string &text, uint16_t defaultPort);
    static Endpoint broadcast(uint16_t port) {
        return Endpoint{0xFFFFFFFFu, port};
    }
    static Endpoint loopback(uint16_t port) {
        return Endpoint{0x7F000001u, port};
    }
};

// Result of a receive call.
enum class RecvResult { Ok, Timeout, Error };

class UdpSocket {
  public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    // Opens the socket and binds to `port` (0 = ephemeral). Enables
    // SO_BROADCAST so hosts can discover the console. Returns false on
    // failure.
    bool open(uint16_t port);
    void close();
    bool isOpen() const { return fd_ != kInvalid; }

    // Port the socket is actually bound to (useful after binding port 0).
    uint16_t boundPort() const;

    // Sets the blocking-receive timeout used by recvFrom.
    bool setRecvTimeout(unsigned ms);

    // Sends one datagram. Returns false on error. Thread-safe with
    // concurrent sends and receives (sendto/recvfrom are kernel-atomic
    // per datagram).
    bool sendTo(const Endpoint &to, const void *data, size_t len);

    // Receives one datagram into `buf`. On Ok, `len` is the payload size
    // and `from` the sender.
    RecvResult recvFrom(void *buf, size_t cap, size_t &len, Endpoint &from);

  private:
#if defined(_WIN32)
    using Handle = uintptr_t;
    static constexpr Handle kInvalid = ~static_cast<Handle>(0);
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    Handle fd_ = kInvalid;
};

} // namespace wsu
