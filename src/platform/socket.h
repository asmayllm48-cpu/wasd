#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace wasd::platform {

bool initSockets();

// Blocking TCP client socket to 127.0.0.1 (adb tunnel / adb server).
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool connectLocal(uint16_t port);
    bool readExact(void* buf, std::size_t n);
    // Reads up to n bytes; returns bytes read, 0 on EOF, -1 on error.
    long long readSome(void* buf, std::size_t n);
    bool writeAll(const void* buf, std::size_t n);
    void setNoDelay(bool on);
    void setRecvBuffer(int bytes);
    void shutdown();  // unblocks readers on other threads
    void close();
    bool valid() const { return s_ != kInvalid; }

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};
    uintptr_t s_ = kInvalid;
};

}  // namespace wasd::platform
