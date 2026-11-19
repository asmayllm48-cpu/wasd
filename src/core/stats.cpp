#include "core/stats.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

namespace wasd {

int64_t nowMicros() {
#ifdef _WIN32
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>((static_cast<long double>(c.QuadPart) * 1000000.0L) / freq);
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

Percentiles computePercentiles(std::vector<double> s) {
    Percentiles p;
    p.count = s.size();
    if (s.empty()) return p;
    std::sort(s.begin(), s.end());
    auto rank = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(s.size()))) ;
        return s[std::clamp<std::size_t>(idx == 0 ? 0 : idx - 1, 0, s.size() - 1)];
    };
    p.p50 = rank(0.50);
    p.p95 = rank(0.95);
    p.p99 = rank(0.99);
    p.max = s.back();
    p.mean = std::accumulate(s.begin(), s.end(), 0.0) / static_cast<double>(s.size());
    return p;
}

SampleWindow::SampleWindow(std::size_t capacity) : capacity_(capacity ? capacity : 1) { data_.reserve(capacity_); }

void SampleWindow::add(double v) {
    std::lock_guard lk(mu_);
    if (!full_) {
        data_.push_back(v);
        if (data_.size() == capacity_) full_ = true;
        return;
    }
    data_[next_] = v;
    next_ = (next_ + 1) % data_.size();
}

std::vector<double> SampleWindow::snapshot() const {
    std::lock_guard lk(mu_);
    return data_;
}

Percentiles SampleWindow::summary() const { return computePercentiles(snapshot()); }

double SampleWindow::latest() const {
    std::lock_guard lk(mu_);
    if (data_.empty()) return 0;
    const std::size_t idx = full_ ? (next_ + data_.size() - 1) % data_.size() : data_.size() - 1;
    return data_[idx];
}

void SampleWindow::clear() {
    std::lock_guard lk(mu_);
    data_.clear();
    next_ = 0;
    full_ = false;
}

void RateCounter::tick(int64_t t) {
    std::lock_guard lk(mu_);
    times_.push_back(t);
    const auto cutoff = t - 1000000;
    times_.erase(times_.begin(), std::find_if(times_.begin(), times_.end(), [&](int64_t x) { return x >= cutoff; }));
}

double RateCounter::rate(int64_t t) const {
    std::lock_guard lk(mu_);
    const auto cutoff = t - 1000000;
    return static_cast<double>(std::count_if(times_.begin(), times_.end(), [&](int64_t x) { return x >= cutoff; }));
}

const char* flightEventTypeName(FlightEventType t) {
    switch (t) {
        case FlightEventType::KeyDown: return "KeyDown";
        case FlightEventType::KeyUp: return "KeyUp";
        case FlightEventType::MouseDelta: return "MouseDelta";
        case FlightEventType::TouchDown: return "TouchDown";
        case FlightEventType::TouchUp: return "TouchUp";
        case FlightEventType::TouchMove: return "TouchMove";
        case FlightEventType::QueueCoalesce: return "QueueCoalesce";
        case FlightEventType::QueueDrop: return "QueueDrop";
        case FlightEventType::QueueStaleDrop: return "QueueStaleDrop";
        case FlightEventType::LatencySpike: return "LatencySpike";
        case FlightEventType::FrameDrop: return "FrameDrop";
        case FlightEventType::MacroStep: return "MacroStep";
        case FlightEventType::SourceConflictResolved: return "SourceConflict";
    }
    return "?";
}

FlightRecorder::FlightRecorder(std::size_t capacity) : ring_(capacity ? capacity : 1) {}

void FlightRecorder::record(FlightEventType type, uint64_t pointerOrKey, int32_t x, int32_t y,
                            int32_t latencyUs, uint32_t sourceMask, int64_t nowUs) {
    const int64_t t = nowUs ? nowUs : nowMicros();
    std::lock_guard lk(mu_);
    ring_[head_] = FlightEvent{t, type, sourceMask, pointerOrKey, x, y, latencyUs};
    head_ = (head_ + 1) % ring_.size();
    if (count_ < ring_.size()) ++count_;
    ++total_;
    if (type == FlightEventType::LatencySpike) ++spikes_;
}

std::vector<FlightEvent> FlightRecorder::snapshot(std::size_t maxCount) const {
    std::lock_guard lk(mu_);
    const std::size_t n = std::min(maxCount, count_);
    std::vector<FlightEvent> out;
    out.reserve(n);
    const std::size_t start = (head_ + ring_.size() - n) % ring_.size();
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(ring_[(start + i) % ring_.size()]);
    }
    return out;
}

void FlightRecorder::clear() {
    std::lock_guard lk(mu_);
    head_ = 0;
    count_ = 0;
    total_ = 0;
    spikes_ = 0;
}

std::size_t FlightRecorder::totalRecorded() const {
    std::lock_guard lk(mu_);
    return total_;
}

std::size_t FlightRecorder::spikeCount() const {
    std::lock_guard lk(mu_);
    return spikes_;
}

bool writeCsv(const std::string& path, const std::vector<CsvRow>& rows, std::string* err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    f << "run_id,device,mode,metric,stage,sample_idx,value_us\n";
    for (const auto& r : rows) {
        f << r.runId << ',' << r.device << ',' << r.mode << ',' << r.metric << ',' << r.stage << ',' << r.index << ','
          << static_cast<int64_t>(std::llround(r.valueUs)) << '\n';
    }
    return static_cast<bool>(f);
}

}  // namespace wasd
