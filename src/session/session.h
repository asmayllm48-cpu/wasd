// One live connection to a phone running the pinned scrcpy-server.
// Threads: startup/video (receive + decode), control sender, control reader.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "core/protocol.h"
#include "core/stats.h"
#include "platform/adb.h"
#include "video/decoder.h"

struct ID3D11Device;

namespace wasd {

enum class SessionState { Idle, Starting, Running, Stopping, Stopped, Failed };
const char* sessionStateName(SessionState s);

struct VideoSettings {
    uint32_t maxSize = 1920;       // longest side, 0 = native
    uint32_t bitRate = 8000000;
    uint32_t maxFps = 0;           // 0 = device max
    bool h265 = false;
    bool hardwareDecode = true;
    std::string encoder;           // optional MediaCodec encoder name
};

struct SessionOptions {
    std::string serial;
    VideoSettings video;
    bool showTouches = false;      // needed by the visual latency probe
    bool stayAwake = true;
};

struct ControlMsg {
    proto::MsgBuffer bytes{};
    uint8_t size = 0;
    bool isMove = false;
    uint64_t pointerId = 0;
    int64_t tInUs = 0;      // T0: OS input arrival time (for latency telemetry)
    int64_t tQueueUs = 0;   // T2: enqueued timestamp (for queue-wait and stale-age)
};

struct SessionCounters {
    std::atomic<uint64_t> videoBytes{0};
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> decodedFrames{0};
    std::atomic<uint64_t> controlSent{0};
    std::atomic<uint64_t> decodeErrors{0};
};

class Session {
public:
    using FrameFn = std::function<void(video::DecodedFrame&&)>;
    using SizeFn = std::function<void(uint32_t w, uint32_t h)>;

    Session(const platform::Adb& adb, std::wstring serverPath);
    ~Session();

    // Non-blocking; progress is observable through state().
    void start(const SessionOptions& opts, ID3D11Device* device, FrameFn onFrame, SizeFn onSize);
    // Blocking; safe to call from any thread except the session's own.
    void stop();

    SessionState state() const { return state_.load(); }
    std::string lastError() const;
    std::string deviceName() const;
    std::string decoderDescription() const;
    const SessionOptions& options() const { return opts_; }
    uint32_t frameWidth() const { return frameW_.load(); }
    uint32_t frameHeight() const { return frameH_.load(); }

    void sendTouch(const proto::TouchEvent& ev, int64_t tInUs);
    void sendKeycode(uint32_t keycode);  // press + release
    void sendRaw(const uint8_t* data, std::size_t n);

    // Diagnostics of the control pipeline.
    void setControlStaleAgeUs(int64_t us);
    std::size_t controlQueueSize() const;
    std::size_t controlCoalesced() const;
    std::size_t controlDropped() const;
    std::size_t controlStaleDropped() const;
    int64_t controlOldestAgeUs() const;

    SessionCounters counters;
    SampleWindow sendLatencyUs{2000};   // T0 input event -> T4 written to control socket
    SampleWindow queueWaitUs{2000};     // T2 enqueue -> T3 dequeue by sender thread
    SampleWindow sendWriteUs{2000};     // T3 socket write start -> T4 write complete
    SampleWindow decodeLatencyUs{2000}; // packet received -> frame decoded
    SampleWindow frameIntervalUs{2000}; // between received video packets
    RateCounter fps;

private:
    void run(ID3D11Device* device);
    bool setup(std::string& err);
    void videoLoop(ID3D11Device* device);
    void controlSendLoop();
    void controlReadLoop();
    void fail(const std::string& err);
    void teardown();

    const platform::Adb& adb_;
    std::wstring serverPath_;
    SessionOptions opts_;
    FrameFn onFrame_;
    SizeFn onSize_;
    std::atomic<SessionState> state_{SessionState::Idle};
    std::atomic<bool> stopRequested_{false};
    mutable std::mutex mu_;
    std::string lastError_, deviceName_, decoderDesc_;
    std::atomic<uint32_t> frameW_{0}, frameH_{0};
    uint32_t codecId_ = 0;
    uint16_t port_ = 0;
    platform::ChildProcess server_;
    platform::TcpSocket video_, control_;
    ControlQueue<ControlMsg> queue_{256};
    std::thread runThread_, sendThread_, readThread_;
};

}  // namespace wasd
