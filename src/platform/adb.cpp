#include "platform/adb.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "core/log.h"

namespace wasd::platform {

std::vector<AdbDevice> parseDeviceList(const std::string& text) {
    std::vector<AdbDevice> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("List of devices", 0) == 0 || line[0] == '*') continue;
        std::istringstream ls(line);
        AdbDevice d;
        if (!(ls >> d.serial >> d.state)) continue;
        std::string tok;
        while (ls >> tok) {
            auto colon = tok.find(':');
            if (colon == std::string::npos) continue;
            const std::string k = tok.substr(0, colon), v = tok.substr(colon + 1);
            if (k == "model") d.model = v;
            else if (k == "usb") d.transport = "usb:" + v;
        }
        out.push_back(std::move(d));
    }
    return out;
}

Adb::Adb(std::wstring adbPath) : path_(std::move(adbPath)) {}

bool Adb::available() const { return GetFileAttributesW(path_.c_str()) != INVALID_FILE_ATTRIBUTES; }

RunResult Adb::run(const std::vector<std::wstring>& args, uint32_t timeoutMs) const {
    return runProcess(path_, args, timeoutMs);
}

RunResult Adb::runFor(const std::string& serial, std::vector<std::wstring> args, uint32_t timeoutMs) const {
    args.insert(args.begin(), {L"-s", widen(serial)});
    return run(args, timeoutMs);
}

bool Adb::startServer(std::string& err) const {
    if (!available()) {
        err = "adb.exe not found: " + narrow(path_);
        return false;
    }
    auto r = run({L"start-server"}, 30000);
    if (!r.started || r.exitCode != 0) {
        err = "adb start-server failed: " + r.output;
        return false;
    }
    return true;
}

std::vector<AdbDevice> Adb::devices(std::string& err) const {
    auto r = run({L"devices", L"-l"});
    if (!r.started || r.exitCode != 0) {
        err = "adb devices failed: " + r.output;
        return {};
    }
    return parseDeviceList(r.output);
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string Adb::getprop(const std::string& serial, const std::string& name) const {
    auto r = runFor(serial, {L"shell", L"getprop", widen(name)});
    return r.exitCode == 0 ? trim(r.output) : std::string();
}

bool Adb::deviceSize(const std::string& serial, uint32_t& w, uint32_t& h) const {
    auto r = runFor(serial, {L"shell", L"wm", L"size"});
    if (!r.started || r.exitCode != 0) return false;
    // Output lines look like "Physical size: 1440x3088" / "Override size: 720x1544".
    uint32_t pw = 0, ph = 0, ow = 0, oh = 0;
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        const bool isOverride = line.find("Override") != std::string::npos;
        const auto colon = line.find("size:");
        uint32_t x = 0, y = 0;
        if (colon == std::string::npos ||
            std::sscanf(line.c_str() + colon + 5, "%ux%u", &x, &y) != 2) continue;
        if (isOverride) { ow = x; oh = y; } else { pw = x; ph = y; }
    }
    w = ow ? ow : pw;
    h = ow ? oh : ph;
    return w != 0;
}

bool Adb::push(const std::string& serial, const std::wstring& local, const std::string& remote, std::string& err) const {
    auto r = runFor(serial, {L"push", local, widen(remote)}, 60000);
    if (!r.started || r.exitCode != 0) {
        err = "adb push failed: " + trim(r.output);
        return false;
    }
    return true;
}

std::string Adb::sha256Remote(const std::string& serial, const std::string& remote) const {
    auto r = runFor(serial, {L"shell", L"sha256sum", widen(remote)});
    if (r.exitCode != 0) return {};
    std::istringstream in(r.output);
    std::string h;
    in >> h;
    return h;
}

std::optional<uint16_t> Adb::forward(const std::string& serial, const std::string& name, std::string& err) const {
    auto r = runFor(serial, {L"forward", L"tcp:0", widen("localabstract:" + name)});
    if (!r.started || r.exitCode != 0) {
        err = "adb forward failed: " + trim(r.output);
        return std::nullopt;
    }
    const long port = std::strtol(trim(r.output).c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) {
        err = "adb forward returned unexpected output: " + r.output;
        return std::nullopt;
    }
    return static_cast<uint16_t>(port);
}

void Adb::removeForward(const std::string& serial, uint16_t port) const {
    runFor(serial, {L"forward", L"--remove", L"tcp:" + std::to_wstring(port)});
}

DeviceTracker::DeviceTracker(const Adb& adb, Callback cb) : adb_(adb), cb_(std::move(cb)) {}
DeviceTracker::~DeviceTracker() { stop(); }

void DeviceTracker::start() {
    if (th_.joinable()) return;
    stop_ = false;
    th_ = std::thread([this] { loop(); });
}

void DeviceTracker::stop() {
    stop_ = true;
    sock_.shutdown();
    if (th_.joinable()) th_.join();
    sock_.close();
}

static bool readHexLen(TcpSocket& s, std::size_t& len) {
    char h[5] = {};
    if (!s.readExact(h, 4)) return false;
    len = std::strtoul(h, nullptr, 16);
    return true;
}

void DeviceTracker::loop() {
    int backoffMs = 250;
    while (!stop_) {
        std::string err;
        if (!adb_.startServer(err)) {
            log::warn(err);
        } else if (sock_.connectLocal(5037)) {
            const std::string req = "host:track-devices-l";
            char hdr[8];
            std::snprintf(hdr, sizeof hdr, "%04x", static_cast<unsigned>(req.size()));
            char status[4];
            if (sock_.writeAll(hdr, 4) && sock_.writeAll(req.data(), req.size()) && sock_.readExact(status, 4) &&
                std::string(status, 4) == "OKAY") {
                backoffMs = 250;
                std::size_t len = 0;
                while (!stop_ && readHexLen(sock_, len)) {
                    if (len > (1 << 20)) break;  // untrusted length guard
                    std::string payload(len, '\0');
                    if (len && !sock_.readExact(payload.data(), len)) break;
                    cb_(parseDeviceList(payload));
                }
            }
            sock_.close();
        }
        if (stop_) break;
        // Reconnect only after the adb server connection drops; bounded backoff, not polling.
        for (int waited = 0; waited < backoffMs && !stop_; waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        backoffMs = std::min(backoffMs * 2, 4000);
    }
}

}  // namespace wasd::platform
