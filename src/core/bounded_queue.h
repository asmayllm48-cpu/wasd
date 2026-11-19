// Bounded queues used between threads. Nothing here grows without limit.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace wasd {

// Holds up to N decoded video frames.
// In Competitive mode (maxDepth = 1), acts as a LatestSlot: older decoded frames
// are immediately replaced so the presenter always shows the freshest frame.
// In Smooth mode (maxDepth = 2..3), buffers up to maxDepth frames to absorb jitter.
template <typename T>
class LatestSlot {
public:
    explicit LatestSlot(std::size_t maxDepth = 1) : maxDepth_(maxDepth ? maxDepth : 1) {}

    void setMaxDepth(std::size_t depth) {
        std::lock_guard lk(mu_);
        maxDepth_ = depth ? depth : 1;
        while (q_.size() > maxDepth_) {
            q_.pop_front();
            ++dropped_;
        }
    }

    std::size_t maxDepth() const {
        std::lock_guard lk(mu_);
        return maxDepth_;
    }

    // Returns the replaced (dropped) item, if any.
    std::optional<T> put(T v) {
        std::lock_guard lk(mu_);
        std::optional<T> droppedItem;
        if (q_.size() >= maxDepth_) {
            droppedItem = std::move(q_.front());
            q_.pop_front();
            ++dropped_;
        }
        q_.push_back(std::move(v));
        return droppedItem;
    }

    std::optional<T> take() {
        std::lock_guard lk(mu_);
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        return v;
    }

    std::size_t size() const {
        std::lock_guard lk(mu_);
        return q_.size();
    }

    std::size_t dropped() const {
        std::lock_guard lk(mu_);
        return dropped_;
    }

private:
    mutable std::mutex mu_;
    std::deque<T> q_;
    std::size_t maxDepth_ = 1;
    std::size_t dropped_ = 0;
};

// FIFO with fixed capacity.
// - Scans backward from the tail to coalesce consecutive MOVE updates for the same pointerId,
//   stopping if a critical state transition (DOWN/UP) for that pointerId is encountered.
// - Evicts stale droppable (MOVE) items older than maxStaleAgeUs so old mouse motion never
//   plays back seconds after the user stopped moving.
// - Never blocks the caller on push(): if full of non-droppable items, expands temporarily
//   up to a hard safety cap (4x capacity) so critical UP/DOWN events are never lost and the
//   input thread is never stalled.
template <typename T>
class ControlQueue {
public:
    explicit ControlQueue(std::size_t capacity, int64_t maxStaleAgeUs = 80000 /*80ms*/)
        : capacity_(capacity ? capacity : 1), maxStaleAgeUs_(maxStaleAgeUs) {}

    void setMaxStaleAgeUs(int64_t maxAgeUs) {
        std::lock_guard lk(mu_);
        maxStaleAgeUs_ = maxAgeUs;
    }

    template <typename Coalesce, typename Droppable>
    bool push(T v, Coalesce coalesce, Droppable droppable) {
        return push(std::move(v), coalesce, droppable, [](const T&, const T&) { return false; },
                    [](const T&) -> int64_t { return 0; }, 0);
    }

    template <typename Coalesce, typename Droppable, typename SameChannel, typename GetTime>
    bool push(T v, Coalesce coalesce, Droppable droppable, SameChannel sameChannel, GetTime getTime, int64_t nowUs) {
        std::unique_lock lk(mu_);
        if (closed_) return false;

        // 1. Prune stale droppable (MOVE) items from the front of the queue if nowUs > 0
        if (nowUs > 0 && maxStaleAgeUs_ > 0 && !q_.empty()) {
            for (auto it = q_.begin(); it != q_.end();) {
                const int64_t itemTime = getTime(*it);
                if (droppable(*it) && itemTime > 0 && (nowUs - itemTime) > maxStaleAgeUs_) {
                    it = q_.erase(it);
                    ++dropped_;
                    ++staleDropped_;
                } else {
                    ++it;
                }
            }
        }

        // 2. Coalesce with the most recent item on the same channel if no non-droppable
        //    transition on that channel intervenes.
        if (droppable(v) && !q_.empty()) {
            for (auto it = q_.rbegin(); it != q_.rend(); ++it) {
                if (coalesce(*it, v)) {
                    *it = std::move(v);
                    ++coalesced_;
                    return true;
                }
                if (sameChannel(*it, v)) {
                    // Hit a critical event (DOWN/UP) on the same pointer; cannot coalesce across it!
                    break;
                }
            }
        }

        // 3. Capacity check & backpressure relief
        if (q_.size() >= capacity_) {
            if (droppable(v)) {
                ++dropped_;
                return false;
            }
            // Non-droppable (critical DOWN/UP/Key): evict oldest droppable item
            for (auto it = q_.begin(); it != q_.end(); ++it) {
                if (droppable(*it)) {
                    q_.erase(it);
                    ++dropped_;
                    break;
                }
            }
            // Never block the input thread! Allow bounded overflow for critical transitions
            if (q_.size() >= capacity_ * 4) {
                ++dropped_;
                return false;
            }
        }
        q_.push_back(std::move(v));
        notEmpty_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock lk(mu_);
        notEmpty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return v;
    }

    // Drains up to maxBatch items at once so the sender thread can write them with minimal lock overhead.
    std::size_t popBatch(std::vector<T>& out, std::size_t maxBatch = 16) {
        std::unique_lock lk(mu_);
        notEmpty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return 0;
        const std::size_t n = std::min(maxBatch, q_.size());
        out.clear();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(std::move(q_.front()));
            q_.pop_front();
        }
        notFull_.notify_all();
        return n;
    }

    template <typename GetTime>
    int64_t oldestAgeUs(int64_t nowUs, GetTime getTime) const {
        std::lock_guard lk(mu_);
        if (q_.empty()) return 0;
        const int64_t t0 = getTime(q_.front());
        return (t0 > 0 && nowUs > t0) ? (nowUs - t0) : 0;
    }

    void close() {
        std::lock_guard lk(mu_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    void reopen() {
        std::lock_guard lk(mu_);
        closed_ = false;
        q_.clear();
    }
    std::size_t size() const {
        std::lock_guard lk(mu_);
        return q_.size();
    }
    std::size_t dropped() const {
        std::lock_guard lk(mu_);
        return dropped_;
    }
    std::size_t staleDropped() const {
        std::lock_guard lk(mu_);
        return staleDropped_;
    }
    std::size_t coalesced() const {
        std::lock_guard lk(mu_);
        return coalesced_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> q_;
    std::size_t capacity_;
    int64_t maxStaleAgeUs_ = 80000;
    std::size_t dropped_ = 0;
    std::size_t staleDropped_ = 0;
    std::size_t coalesced_ = 0;
    bool closed_ = false;
};

}  // namespace wasd
