// Measurement helpers. Values reported in the UI come only from these samples.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wasd {

int64_t nowMicros();  // monotonic (QueryPerformanceCounter on Windows)

struct Percentiles {
    std::size_t count = 0;
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};

// Nearest-rank percentiles over a copy of the samples.
Percentiles computePercentiles(std::vector<double> samples);

// Fixed-capacity ring of samples; thread-safe.
class SampleWindow {
public:
    explicit SampleWindow(std::size_t capacity = 600);
    void add(double v);
    Percentiles summary() const;
    std::vector<double> snapshot() const;
    double latest() const;  // most recent sample, 0 when empty
    void clear();

private:
    mutable std::mutex mu_;
    std::size_t capacity_;
    std::vector<double> data_;
    std::size_t next_ = 0;
    bool full_ = false;
};

// Counts events per second over a sliding one-second window.
class RateCounter {
public:
    void tick(int64_t tMicros);
    double rate(int64_t tMicros) const;

private:
    mutable std::mutex mu_;
    std::vector<int64_t> times_;
};

// Detailed stage timestamps (microseconds, monotonic QPC) for an input/video cycle:
// T0: OS event arrival (WM_INPUT / WM_KEYDOWN)
// T1: InputEngine processing completed -> control command emitted
// T2: Enqueued into ControlQueue
// T3: Dequeued by control sender thread, socket write starting
// T4: Socket write completed (transport level)
// T5: Related video packet received (rxUs)
// T6: Video frame decode completed (decUs)
// T7: Frame uploaded & submitted to D3D11 render pipeline
// T8: SwapChain Present completed
struct PipelineTimestamps {
    int64_t t0_arrivalUs = 0;
    int64_t t1_processedUs = 0;
    int64_t t2_queuedUs = 0;
    int64_t t3_sendStartUs = 0;
    int64_t t4_sendDoneUs = 0;
    int64_t t5_videoRxUs = 0;
    int64_t t6_decodeDoneUs = 0;
    int64_t t7_renderSubmitUs = 0;
    int64_t t8_presentDoneUs = 0;
};

enum class FlightEventType : uint8_t {
    KeyDown = 0,
    KeyUp = 1,
    MouseDelta = 2,
    TouchDown = 3,
    TouchUp = 4,
    TouchMove = 5,
    QueueCoalesce = 6,
    QueueDrop = 7,
    QueueStaleDrop = 8,
    LatencySpike = 9,
    FrameDrop = 10,
    MacroStep = 11,
    SourceConflictResolved = 12,
};

const char* flightEventTypeName(FlightEventType t);

struct FlightEvent {
    int64_t timestampUs = 0;
    FlightEventType type = FlightEventType::KeyDown;
    uint32_t sourceMask = 0;   // bitmask of InputSource active
    uint64_t pointerOrKey = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t latencyUs = 0;     // stage or total latency if applicable
};

// Fixed-size, allocation-free in-memory circular buffer for capturing the last N events
// when a hitch or latency spike occurs, without disk I/O on the hot path.
class FlightRecorder {
public:
    static constexpr std::size_t kDefaultCapacity = 1024;
    explicit FlightRecorder(std::size_t capacity = kDefaultCapacity);

    void record(FlightEventType type, uint64_t pointerOrKey, int32_t x = 0, int32_t y = 0,
                int32_t latencyUs = 0, uint32_t sourceMask = 0, int64_t nowUs = 0);
    std::vector<FlightEvent> snapshot(std::size_t maxCount = kDefaultCapacity) const;
    void clear();
    std::size_t totalRecorded() const;
    std::size_t spikeCount() const;

private:
    mutable std::mutex mu_;
    std::vector<FlightEvent> ring_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t total_ = 0;
    std::size_t spikes_ = 0;
};

struct CsvRow {
    std::string runId, device, mode, metric, stage;
    std::size_t index = 0;
    double valueUs = 0;
};

bool writeCsv(const std::string& path, const std::vector<CsvRow>& rows, std::string* err = nullptr);

}  // namespace wasd
