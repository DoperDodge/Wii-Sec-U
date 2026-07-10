// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include "net/udp_socket.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace wsu {

namespace {

#if defined(_WIN32)
// WinSock needs process-wide startup; keep a refcount-free one-shot
// initializer alive for the process lifetime.
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};
void ensureWinsock() { static WinsockInit init; }
#else
void ensureWinsock() {}
#endif

sockaddr_in toSockaddr(const Endpoint &ep) {
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ep.addr);
    sa.sin_port = htons(ep.port);
    return sa;
}

Endpoint fromSockaddr(const sockaddr_in &sa) {
    Endpoint ep;
    ep.addr = ntohl(sa.sin_addr.s_addr);
    ep.port = ntohs(sa.sin_port);
    return ep;
}

} // namespace

std::string Endpoint::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (addr >> 24) & 0xFF,
                  (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
                  port);
    return buf;
}

Endpoint Endpoint::parse(const std::string &text, uint16_t defaultPort) {
    Endpoint ep;
    unsigned a, b, c, d;
    unsigned port = defaultPort;
    int fields = std::sscanf(text.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d,
                             &port);
    if (fields < 4 || a > 255 || b > 255 || c > 255 || d > 255 ||
        port == 0 || port > 65535) {
        return Endpoint{};
    }
    ep.addr = (a << 24) | (b << 16) | (c << 8) | d;
    ep.port = static_cast<uint16_t>(port);
    return ep;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t port) {
    ensureWinsock();
    close();
#if defined(_WIN32)
    fd_ = static_cast<Handle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#else
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (fd_ == kInvalid) return false;

    int on = 1;
    setsockopt(static_cast<int>(fd_), SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char *>(&on), sizeof(on));
    setsockopt(static_cast<int>(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&on), sizeof(on));

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (::bind(static_cast<int>(fd_), reinterpret_cast<sockaddr *>(&sa),
               sizeof(sa)) != 0) {
        close();
        return false;
    }
    return true;
}

void UdpSocket::close() {
    if (fd_ == kInvalid) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd_));
#else
    ::close(fd_);
#endif
    fd_ = kInvalid;
}

uint16_t UdpSocket::boundPort() const {
    if (fd_ == kInvalid) return 0;
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
#if defined(_WIN32)
    int len = sizeof(sa);
#else
    socklen_t len = sizeof(sa);
#endif
    if (getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr *>(&sa),
                    &len) != 0) {
        return 0;
    }
    return ntohs(sa.sin_port);
}

bool UdpSocket::setRecvTimeout(unsigned ms) {
    if (fd_ == kInvalid) return false;
#if defined(_WIN32)
    DWORD timeout = ms;
    return setsockopt(static_cast<SOCKET>(fd_), SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char *>(&timeout),
                      sizeof(timeout)) == 0;
#else
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<long>(ms % 1000) * 1000;
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UdpSocket::sendTo(const Endpoint &to, const void *data, size_t len) {
    if (fd_ == kInvalid || !to.valid()) return false;
    sockaddr_in sa = toSockaddr(to);
#if defined(_WIN32)
    int sent = ::sendto(static_cast<SOCKET>(fd_),
                        static_cast<const char *>(data),
                        static_cast<int>(len), 0,
                        reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
#else
    ssize_t sent = ::sendto(fd_, data, len, 0,
                            reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
#endif
    return sent == static_cast<decltype(sent)>(len);
}

RecvResult UdpSocket::recvFrom(void *buf, size_t cap, size_t &len,
                               Endpoint &from) {
    if (fd_ == kInvalid) return RecvResult::Error;
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
#if defined(_WIN32)
    int salen = sizeof(sa);
    int got = ::recvfrom(static_cast<SOCKET>(fd_), static_cast<char *>(buf),
                         static_cast<int>(cap), 0,
                         reinterpret_cast<sockaddr *>(&sa), &salen);
    if (got < 0) {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            return RecvResult::Timeout;
        }
        // A previous send to an unreachable port surfaces here on Windows;
        // treat it as a transient miss rather than a socket failure.
        if (err == WSAECONNRESET) return RecvResult::Timeout;
        return RecvResult::Error;
    }
#else
    socklen_t salen = sizeof(sa);
    ssize_t got = ::recvfrom(fd_, buf, cap, 0,
                             reinterpret_cast<sockaddr *>(&sa), &salen);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return RecvResult::Timeout;
        }
        return RecvResult::Error;
    }
#endif
    len = static_cast<size_t>(got);
    from = fromSockaddr(sa);
    return RecvResult::Ok;
}

} // namespace wsu
