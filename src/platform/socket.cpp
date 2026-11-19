#include "platform/socket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <utility>

namespace wasd::platform {

bool initSockets() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& o) noexcept : s_(std::exchange(o.s_, kInvalid)) {}
TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        s_ = std::exchange(o.s_, kInvalid);
    }
    return *this;
}

bool TcpSocket::connectLocal(uint16_t port) {
    close();
    initSockets();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return false;
    }
    s_ = static_cast<uintptr_t>(s);
    return true;
}

bool TcpSocket::readExact(void* buf, std::size_t n) {
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        int r = ::recv(static_cast<SOCKET>(s_), p, static_cast<int>(n > (1 << 30) ? (1 << 30) : n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

long long TcpSocket::readSome(void* buf, std::size_t n) {
    int r = ::recv(static_cast<SOCKET>(s_), static_cast<char*>(buf), static_cast<int>(n), 0);
    return r < 0 ? -1 : r;
}

bool TcpSocket::writeAll(const void* buf, std::size_t n) {
    auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        int r = ::send(static_cast<SOCKET>(s_), p, static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

void TcpSocket::setNoDelay(bool on) {
    BOOL v = on ? TRUE : FALSE;
    setsockopt(static_cast<SOCKET>(s_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof v);
}

void TcpSocket::setRecvBuffer(int bytes) {
    setsockopt(static_cast<SOCKET>(s_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof bytes);
}

void TcpSocket::shutdown() {
    if (valid()) ::shutdown(static_cast<SOCKET>(s_), SD_BOTH);
}

void TcpSocket::close() {
    if (valid()) {
        closesocket(static_cast<SOCKET>(s_));
        s_ = kInvalid;
    }
}

}  // namespace wasd::platform
