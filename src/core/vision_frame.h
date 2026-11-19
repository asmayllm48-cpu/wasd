// Frame identity + inference scheduling for the vision pipeline.
//
// Every analyzed frame carries a FrameStamp so results can never be mistaken
// for "current" data. Inference runs on a Latest-Frame policy: the mailbox
// holds at most ONE pending job — a newer frame replaces the stale one, so
// scan latency can never accumulate. The scheduler picks the analysis rate
// (fixed presets or Auto); Auto adapts to measured inference p95 with
// hysteresis and a minimum dwell so it doesn't flap on single spikes.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace wasd {

// Monotonic stamp attached to every analyzed frame. `captureUs` is the moment
// the source pixels were known-good (readback landed — the closest honest
// timestamp available below the video stream).
struct FrameStamp {
    uint64_t id = 0;       // monotonically increasing frame counter
    int64_t captureUs = 0; // source-timestamp of the pixels
    uint32_t w = 0, h = 0;
};

// Stamps carried end-to-end by an inference result.
struct InferStamp : FrameStamp {
    int64_t inferStartUs = 0;
    int64_t inferEndUs = 0;
    int64_t inferUs = 0;     // duration
    int64_t resultUs = 0;    // drained on the consumer thread
    int64_t ageAtResultUs = 0;
};

// Single-slot latest-wins mailbox: push() overwrites a pending item and counts
// it as skipped. The consumer never processes a queue of stale frames.
template <typename T>
class LatestMailbox {
public:
    void push(T v) {
        std::lock_guard lk(m_);
        if (pending_) ++skipped_;
        box_ = std::move(v);
        pending_ = true;
    }
    bool take(T& out) {
        std::lock_guard lk(m_);
        if (!pending_) return false;
        out = std::move(box_);
        pending_ = false;
        return true;
    }
    uint64_t skipped() const { return skipped_.load(); }
private:
    std::mutex m_;
    T box_{};
    bool pending_ = false;
    std::atomic<uint64_t> skipped_{0};
};

// Fixed rate presets (fps) offered by the UI; 0 = Auto.
inline constexpr int kInferRatePresets[] = {15, 20, 30, 45, 60};

// Inference-rate governor. Fixed mode: period = 1/fps. Auto mode: derives the
// rate from measured inference p95 — it must allow ~2 idle scan slots per
// inference — then moves within ±1 preset step per decision window with a
// minimum dwell and a deadband so the rate doesn't flap.
class InferScheduler {
public:
    void setMode(int fps) { modeFps_ = fps; }   // 0 = Auto
    int mode() const { return modeFps_; }
    int effectiveFps() const { return modeFps_ > 0 ? modeFps_ : autoFps_; }
    int64_t periodUs() const { return 1000000 / std::max(1, effectiveFps()); }
    uint64_t skippedByRate() const { return skippedByRate_; }

    // Should we launch a scan now? `now` in microseconds.
    bool due(int64_t now, bool busy) {
        if (busy) return false;
        if (now - lastLaunch_ < periodUs()) { ++skippedByRate_; return false; }
        lastLaunch_ = now;
        return true;
    }

    // Feed one completed inference duration; Auto re-evaluates each window.
    void noteInferUs(int64_t inferUs, int64_t now) {
        recent_.push_back(inferUs);
        if (recent_.size() > 32) recent_.pop_front();
        if (modeFps_ != 0 || now - lastAutoAt_ < kDwellUs) return;
        lastAutoAt_ = now;
        std::deque<int64_t> s = recent_;
        std::sort(s.begin(), s.end());
        const int64_t p95 = s.empty() ? 0 : s[size_t(s.size() * 0.95f)];
        // Budget: inference should occupy < ~50% of a scan slot.
        int target = p95 > 0 ? int(500000 / std::max<int64_t>(p95, 1)) : 60;
        target = std::clamp(target, 15, 60);
        // Hysteresis: move at most one preset step, only if clearly different.
        if (autoFps_ == 0) autoFps_ = nearestPreset(target);
        else if (target >= autoFps_ * 1.25f) autoFps_ = nearestPresetUp(autoFps_);
        else if (target <= autoFps_ * 0.80f) autoFps_ = nearestPresetDown(autoFps_);
    }

    int64_t lastP95Us() const {
        if (recent_.empty()) return 0;
        std::deque<int64_t> s = recent_;
        std::sort(s.begin(), s.end());
        return s[size_t(s.size() * 0.95f)];
    }

private:
    static constexpr int64_t kDwellUs = 1500000;  // >=1.5s between Auto changes
    int modeFps_ = 30;
    int autoFps_ = 30;
    int64_t lastLaunch_ = -100000000000LL;  // first call is always due
    int64_t lastAutoAt_ = 0;
    std::deque<int64_t> recent_;
    uint64_t skippedByRate_ = 0;

    static int nearestPreset(int fps) {
        int best = kInferRatePresets[0];
        for (int p : kInferRatePresets) if (std::abs(p - fps) < std::abs(best - fps)) best = p;
        return best;
    }
    static int nearestPresetUp(int cur) {
        for (int p : kInferRatePresets) if (p > cur) return p;
        return cur;
    }
    static int nearestPresetDown(int cur) {
        int best = cur;
        for (int p : kInferRatePresets) if (p < cur) best = p;
        return best;
    }
};

}  // namespace wasd
