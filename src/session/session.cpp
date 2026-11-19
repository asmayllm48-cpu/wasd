#include "session/session.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>
#include <vector>

#include "core/log.h"
#include "platform/paths.h"

namespace wasd {
using namespace std::chrono_literals;
using platform::widen;

static constexpr const char* kRemoteServerPath = "/data/local/tmp/wasd-scrcpy-server-5.0.1.jar";
static constexpr const char* kPinnedServerSha256 = "764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536";

const char* sessionStateName(SessionState s) {
    switch (s) {
        case SessionState::Idle: return "Idle";
        case SessionState::Starting: return "Starting";
        case SessionState::Running: return "Running";
        case SessionState::Stopping: return "Stopping";
        case SessionState::Stopped: return "Stopped";
        case SessionState::Failed: return "Failed";
    }
    return "?";
}

Session::Session(const platform::Adb& adb, std::wstring serverPath) : adb_(adb), serverPath_(std::move(serverPath)) {}
Session::~Session() { stop(); }

std::string Session::lastError() const {
    std::lock_guard lk(mu_);
    return lastError_;
}
std::string Session::deviceName() const {
    std::lock_guard lk(mu_);
    return deviceName_;
}
std::string Session::decoderDescription() const {
    std::lock_guard lk(mu_);
    return decoderDesc_;
}

void Session::fail(const std::string& err) {
    {
        std::lock_guard lk(mu_);
        if (lastError_.empty()) lastError_ = err;
    }
    log::error("session: " + err);
}

void Session::start(const SessionOptions& opts, ID3D11Device* device, FrameFn onFrame, SizeFn onSize) {
    stop();
    opts_ = opts;
    onFrame_ = std::move(onFrame);
    onSize_ = std::move(onSize);
    {
        std::lock_guard lk(mu_);
        lastError_.clear();
        deviceName_.clear();
        decoderDesc_.clear();
    }
    frameW_ = frameH_ = 0;
    stopRequested_ = false;
    queue_.reopen();
    state_ = SessionState::Starting;
    runThread_ = std::thread([this, device] { run(device); });
}

void Session::stop() {
    if (runThread_.joinable() && std::this_thread::get_id() != runThread_.get_id()) {
        stopRequested_ = true;
        if (state_ == SessionState::Running || state_ == SessionState::Starting) state_ = SessionState::Stopping;
        queue_.close();
        video_.shutdown();
        control_.shutdown();
        server_.kill();
        runThread_.join();
    }
}

bool Session::setup(std::string& err) {
    const std::string& serial = opts_.serial;
    const std::string localHash = platform::sha256File(serverPath_);
    if (localHash != kPinnedServerSha256) {
        err = "scrcpy-server hash mismatch or file missing (" + platform::narrow(serverPath_) + ")";
        return false;
    }
    if (adb_.sha256Remote(serial, kRemoteServerPath) != kPinnedServerSha256) {
        log::info("pushing server to device");
        if (!adb_.push(serial, serverPath_, kRemoteServerPath, err)) return false;
        if (adb_.sha256Remote(serial, kRemoteServerPath) != kPinnedServerSha256) {
            err = "server hash on device does not match after push";
            return false;
        }
    }
    std::random_device rd;
    const uint32_t scid = rd() & 0x7FFFFFFF;
    char scidHex[16], name[32];
    std::snprintf(scidHex, sizeof scidHex, "%08x", scid);
    std::snprintf(name, sizeof name, "scrcpy_%08x", scid);
    auto port = adb_.forward(serial, name, err);
    if (!port) return false;
    port_ = *port;

    const VideoSettings& v = opts_.video;
    std::vector<std::wstring> args = {L"-s", widen(serial), L"shell", widen(std::string("CLASSPATH=") + kRemoteServerPath),
                                      L"app_process", L"/", L"com.genymobile.scrcpy.Server", widen(proto::kServerVersion),
                                      widen(std::string("scid=") + scidHex), L"log_level=info", L"audio=false",
                                      L"tunnel_forward=true", L"control=true", L"clipboard_autosync=false", L"cleanup=true",
                                      v.h265 ? L"video_codec=h265" : L"video_codec=h264",
                                      L"max_size=" + std::to_wstring(v.maxSize),
                                      L"video_bit_rate=" + std::to_wstring(v.bitRate),
                                      opts_.showTouches ? L"show_touches=true" : L"show_touches=false",
                                      opts_.stayAwake ? L"stay_awake=true" : L"stay_awake=false"};
    if (v.maxFps) args.push_back(L"max_fps=" + std::to_wstring(v.maxFps));
    if (!v.encoder.empty()) args.push_back(widen("video_encoder=" + v.encoder));
    if (!server_.start(adb_.path(), args, [](const std::string& line) { log::info("[server] " + line); })) {
        err = "failed to start adb shell for server";
        return false;
    }

    // With a forward tunnel, adb accepts the TCP connection even before the server
    // listens; the dummy byte confirms the real connection (same as scrcpy client).
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !stopRequested_; ++attempt) {
        if (video_.connectLocal(port_)) {
            uint8_t dummy;
            if (video_.readExact(&dummy, 1)) {
                connected = true;
                break;
            }
            video_.close();
        }
        if (!server_.running()) break;
        std::this_thread::sleep_for(100ms);
    }
    if (!connected) {
        err = stopRequested_ ? "cancelled" : "could not connect to server (see log for server output)";
        return false;
    }
    if (!control_.connectLocal(port_)) {
        err = "could not open control socket";
        return false;
    }
    control_.setNoDelay(true);
    video_.setRecvBuffer(1 << 20);

    char devName[proto::kDeviceNameLength + 1] = {};
    if (!video_.readExact(devName, proto::kDeviceNameLength)) {
        err = "failed to read device name";
        return false;
    }
    uint8_t codec[4];
    if (!video_.readExact(codec, 4)) {
        err = "failed to read codec id";
        return false;
    }
    codecId_ = proto::readU32(codec);
    if (codecId_ == proto::kStreamDisabled || codecId_ == proto::kStreamError) {
        err = "device could not start the video encoder (try lower resolution or another codec)";
        return false;
    }
    std::lock_guard lk(mu_);
    deviceName_ = devName;
    return true;
}

void Session::run(ID3D11Device* device) {
    // The session/video thread drives decode and frame delivery. Keep it above normal
    // so video keeps flowing (and the frame event keeps firing) even when the UI thread
    // is busy drawing, but below the control sender so input never waits on video.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::string err;
    if (!setup(err)) {
        fail(err);
        teardown();
        state_ = stopRequested_ ? SessionState::Stopped : SessionState::Failed;
        return;
    }
    log::info("session connected: " + deviceName());
    state_ = SessionState::Running;
    sendThread_ = std::thread([this] { controlSendLoop(); });
    readThread_ = std::thread([this] { controlReadLoop(); });
    videoLoop(device);
    teardown();
    state_ = stopRequested_ ? SessionState::Stopped : SessionState::Failed;
    log::info(std::string("session ended: ") + sessionStateName(state_));
}

void Session::videoLoop(ID3D11Device* device) {
    video::VideoDecoder decoder;
    std::string err;
    if (!decoder.open(codecId_, device, opts_.video.hardwareDecode, err)) {
        fail(err);
        return;
    }
    {
        std::lock_guard lk(mu_);
        decoderDesc_ = decoder.description();
    }
    std::vector<uint8_t> config, packet;
    packet.reserve(1 << 20);
    int64_t lastRx = 0;
    std::array<uint8_t, proto::kHeaderSize> hdr;
    constexpr uint32_t kMaxPacket = 32u << 20;  // untrusted length guard
    while (!stopRequested_) {
        if (!video_.readExact(hdr.data(), hdr.size())) {
            if (!stopRequested_) fail("video stream closed by device (disconnected?)");
            return;
        }
        const proto::StreamHeader h = proto::parseHeader(hdr);
        if (h.isSession) {
            // Mid-stream size change (rotation, fold): re-open the decoder so the
            // hardware path re-initializes for the new dimensions. Without this,
            // FFmpeg's lazy hwaccel setup fails on the resize and the session
            // silently degrades to software decoding for the rest of its life.
            if (frameW_ && (h.session.width != frameW_ || h.session.height != frameH_)) {
                std::string oerr;
                if (!decoder.open(codecId_, device, opts_.video.hardwareDecode, oerr)) {
                    fail(oerr);
                    return;
                }
                // The reopened decoder needs a keyframe before it can produce a
                // frame again; RESET_VIDEO makes the server emit one immediately
                // instead of waiting for the encoder's natural keyframe interval.
                uint8_t rst[proto::kMaxMsgSize];
                sendRaw(rst, proto::serializeResetVideo(rst));
            }
            frameW_ = h.session.width;
            frameH_ = h.session.height;
            log::info("video size " + std::to_string(h.session.width) + "x" + std::to_string(h.session.height));
            if (onSize_) onSize_(h.session.width, h.session.height);
            continue;
        }
        if (h.media.size == 0 || h.media.size > kMaxPacket) {
            fail("invalid packet size from device");
            return;
        }
        const std::size_t offset = h.media.config ? 0 : config.size();
        packet.resize(offset + h.media.size);
        if (offset) std::memcpy(packet.data(), config.data(), offset);
        if (!video_.readExact(packet.data() + offset, h.media.size)) {
            if (!stopRequested_) fail("video stream interrupted");
            return;
        }
        const int64_t rx = nowMicros();
        counters.videoBytes += h.media.size;
        ++counters.packets;
        if (h.media.config) {
            config = packet;  // merged into the next media packet
            continue;
        }
        config.clear();
        if (lastRx) frameIntervalUs.add(static_cast<double>(rx - lastRx));
        lastRx = rx;
        const bool ok = decoder.decode(packet.data(), packet.size(), h.media.pts, h.media.keyFrame, rx,
                                       [this](video::DecodedFrame&& f) {
                                           ++counters.decodedFrames;
                                           fps.tick(f.decUs);
                                           decodeLatencyUs.add(static_cast<double>(f.decUs - f.rxUs));
                                           if (onFrame_) onFrame_(std::move(f));
                                       });
        if (!ok) {
            ++counters.decodeErrors;
            fail("fatal decoder error");
            return;
        }
    }
}

void Session::controlSendLoop() {
    // The control sender is the most latency-critical thread in the pipeline:
    // touch/key transitions must reach the device as soon as they are produced.
    // TIME_CRITICAL keeps it ahead of decode and UI under load.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<ControlMsg> batch;
    batch.reserve(16);
    while (!stopRequested_) {
        const std::size_t n = queue_.popBatch(batch, 16);
        if (n == 0) return;  // queue closed (session stopping)
        const int64_t t3 = nowMicros();
        // Write the whole batch in one send() when possible: touch/key messages are
        // already serialized in order into their own 32-byte buffers.
        for (std::size_t i = 0; i < n; ++i) {
            const ControlMsg& m = batch[i];
            if (m.tQueueUs) queueWaitUs.add(static_cast<double>(t3 - m.tQueueUs));
        }
        // Single socket write for the batch keeps ordering and cuts syscall count.
        uint8_t buf[proto::kMaxMsgSize * 16];
        std::size_t off = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::memcpy(buf + off, batch[i].bytes.data(), batch[i].size);
            off += batch[i].size;
        }
        if (!control_.writeAll(buf, off)) {
            if (!stopRequested_) fail("control socket write failed");
            video_.shutdown();
            return;
        }
        const int64_t t4 = nowMicros();
        sendWriteUs.add(static_cast<double>(t4 - t3));
        counters.controlSent += n;
        for (std::size_t i = 0; i < n; ++i) {
            if (batch[i].tInUs) sendLatencyUs.add(static_cast<double>(t4 - batch[i].tInUs));
        }
    }
}

void Session::controlReadLoop() {
    // Device -> PC messages (clipboard, acks, uhid). Clipboard sync is disabled; drain and discard.
    uint8_t buf[4096];
    while (!stopRequested_) {
        const long long n = control_.readSome(buf, sizeof buf);
        if (n <= 0) return;
    }
}

void Session::teardown() {
    queue_.close();
    if (control_.valid()) {
        // Best effort: lift any finger the server may still consider down is handled by
        // the caller (InputEngine::releaseAll) before stop(); the server also resets on close.
        control_.shutdown();
    }
    video_.shutdown();
    if (sendThread_.joinable()) sendThread_.join();
    if (readThread_.joinable()) readThread_.join();
    video_.close();
    control_.close();
    server_.kill();
    if (port_) {
        adb_.removeForward(opts_.serial, port_);
        port_ = 0;
    }
}

void Session::sendTouch(const proto::TouchEvent& ev, int64_t tInUs) {
    if (state_ != SessionState::Running) return;
    ControlMsg m;
    m.size = static_cast<uint8_t>(proto::serializeTouch(ev, m.bytes.data()));
    m.isMove = ev.action == proto::MotionAction::Move;
    m.pointerId = ev.pointerId;
    m.tInUs = tInUs;
    m.tQueueUs = nowMicros();
    queue_.push(
        std::move(m),
        [](const ControlMsg& a, const ControlMsg& b) { return a.isMove && b.isMove && a.pointerId == b.pointerId; },
        [](const ControlMsg& c) { return c.isMove; },
        [](const ControlMsg& a, const ControlMsg& b) { return a.pointerId == b.pointerId; },
        [](const ControlMsg& c) { return c.tQueueUs; },
        m.tQueueUs);
}

void Session::sendRaw(const uint8_t* data, std::size_t n) {
    if (state_ != SessionState::Running || n > proto::kMaxMsgSize) return;
    ControlMsg m;
    std::memcpy(m.bytes.data(), data, n);
    m.size = static_cast<uint8_t>(n);
    m.tQueueUs = nowMicros();
    queue_.push(
        std::move(m), [](const ControlMsg&, const ControlMsg&) { return false; },
        [](const ControlMsg&) { return false; },
        [](const ControlMsg&, const ControlMsg&) { return false; },
        [](const ControlMsg& c) { return c.tQueueUs; },
        m.tQueueUs);
}

void Session::setControlStaleAgeUs(int64_t us) { queue_.setMaxStaleAgeUs(us); }
std::size_t Session::controlQueueSize() const { return const_cast<ControlQueue<ControlMsg>&>(queue_).size(); }
std::size_t Session::controlCoalesced() const { return const_cast<ControlQueue<ControlMsg>&>(queue_).coalesced(); }
std::size_t Session::controlDropped() const { return const_cast<ControlQueue<ControlMsg>&>(queue_).dropped(); }
std::size_t Session::controlStaleDropped() const { return const_cast<ControlQueue<ControlMsg>&>(queue_).staleDropped(); }
int64_t Session::controlOldestAgeUs() const {
    return const_cast<ControlQueue<ControlMsg>&>(queue_).oldestAgeUs(nowMicros(),
                                                                    [](const ControlMsg& c) { return c.tQueueUs; });
}

void Session::sendKeycode(uint32_t keycode) {
    uint8_t b[proto::kMaxMsgSize];
    sendRaw(b, proto::serializeKeycode(proto::KeyAction::Down, keycode, 0, 0, b));
    sendRaw(b, proto::serializeKeycode(proto::KeyAction::Up, keycode, 0, 0, b));
}

}  // namespace wasd
