// Thin wrapper over the bundled adb.exe. Only documented adb commands are used.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.h"
#include "platform/socket.h"

namespace wasd::platform {

struct AdbDevice {
    std::string serial;
    std::string state;  // device, unauthorized, offline, ...
    std::string model;
    std::string transport;  // usb:..., or empty
    bool usable() const { return state == "device"; }
};

// Parses `adb devices -l` / track-devices-l payload lines.
std::vector<AdbDevice> parseDeviceList(const std::string& text);

class Adb {
public:
    explicit Adb(std::wstring adbPath);
    const std::wstring& path() const { return path_; }
    bool available() const;

    RunResult run(const std::vector<std::wstring>& args, uint32_t timeoutMs = 20000) const;
    RunResult runFor(const std::string& serial, std::vector<std::wstring> args, uint32_t timeoutMs = 20000) const;

    bool startServer(std::string& err) const;
    std::vector<AdbDevice> devices(std::string& err) const;
    std::string getprop(const std::string& serial, const std::string& name) const;
    // `wm size`: parses "Physical size: WxH" / "Override size: WxH" (prefers the
    // override). Returned in the device's natural (portrait) orientation.
    bool deviceSize(const std::string& serial, uint32_t& w, uint32_t& h) const;
    bool push(const std::string& serial, const std::wstring& local, const std::string& remote, std::string& err) const;
    std::string sha256Remote(const std::string& serial, const std::string& remote) const;
    // `adb forward tcp:0 localabstract:<name>`; returns the allocated port.
    std::optional<uint16_t> forward(const std::string& serial, const std::string& abstractName, std::string& err) const;
    void removeForward(const std::string& serial, uint16_t port) const;

private:
    std::wstring path_;
};

// Event-based device monitoring via the adb server's `host:track-devices-l`
// service on 127.0.0.1:5037 (documented in adb's SERVICES.TXT / protocol.txt).
class DeviceTracker {
public:
    using Callback = std::function<void(const std::vector<AdbDevice>&)>;
    DeviceTracker(const Adb& adb, Callback cb);
    ~DeviceTracker();
    void start();
    void stop();

private:
    void loop();
    const Adb& adb_;
    Callback cb_;
    std::atomic<bool> stop_{false};
    TcpSocket sock_;
    std::thread th_;
};

}  // namespace wasd::platform
