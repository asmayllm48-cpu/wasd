// Sticky object tracker with an explicit lifecycle state machine.
//
// Every track carries TWO different things that the old code fused into one:
//   - measBox: the last REAL detection (never extrapolated, never blended
//     into ghost positions)
//   - box: the displayed/steering box (smoothed, and glided by velocity
//     between scans via predicted())
//
// Lifecycle: Confirmed -> TempLost -> Reacquiring -> Confirmed, with Expired
// as the terminal state. A lost track can only be re-confirmed by FRESH,
// CONSISTENT evidence (kReacqScans consecutive matching detections) — never by
// its own prediction. New candidates also start unconfirmed, so a single-scan
// false positive can never steer the aim or pose as a real target.
//
// State transitions are recorded in `events` (bounded ring) with their reason,
// so a ghost cycle (Confirmed->Lost->Reacq->...) is visible in diagnostics
// instead of being silent.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "core/automation.h"
#include "core/coords.h"
#include "core/detector.h"

namespace wasd {

enum class TrackState : uint8_t {
    Confirmed,     // supported by the latest scan(s); safe to steer/display as real
    TempLost,      // scan(s) missed; prediction glides but is NOT evidence
    Reacquiring,   // fresh evidence seen, not yet consistent enough
    Expired        // dead — emitted to the log, removed from tracks
};

enum class TrackReason : uint8_t {
    Spawned,         // candidate created from an unmatched detection
    Reconfirmed,     // gathered enough consecutive evidence after a loss
    MissedScan,      // a scan contained no matching detection
    ReacquireHit,    // first fresh hit while lost
    ExpiredMissed,   // too many consecutive misses
    ExpiredAge,      // no real measurement for too long
    Cleared          // tracker reset (watch stopped)
};

struct DetectTrack {
    int classId = -1;
    uint32_t id = 0;
    Rect box{};            // smoothed displayed box (center+size smoothed separately)
    Rect measBox{};        // LAST REAL MEASUREMENT — never extrapolated
    float vx = 0, vy = 0;  // center velocity (normalized units/sec)
    float conf = 0;
    bool boosted = false;
    int64_t lastMeasUs = 0;   // timestamp of measBox
    int64_t stateSinceUs = 0; // entered current state
    int missed = 0;           // consecutive scans without a match
    int reacqCount = 0;       // consecutive evidence hits while reacquiring
    TrackState state = TrackState::Reacquiring;
};

struct TrackEvent {
    int64_t tUs = 0;
    uint32_t id = 0;
    TrackState from = TrackState::Reacquiring;
    TrackState to = TrackState::Reacquiring;
    TrackReason why = TrackReason::Spawned;
};

class DetectTracker {
public:
    // How many consecutive matching scans re-confirm a lost track, and confirm
    // a newborn candidate. 2 scans ~= 40-100 ms of proof — cheap insurance
    // against single-scan false positives steering the aim.
    static constexpr int kReacqScans = 2;
    // Velocity kept per missed scan: halves the ghost glide each empty scan so
    // a stale pan estimate dies fast instead of dragging the aim point away.
    static constexpr float kVelDecayPerMiss = 0.5f;

    std::vector<DetectTrack> tracks;
    std::deque<TrackEvent> events;  // bounded at 128

    // `protectId`: a locked/important track gets a larger missed-scan budget
    // (kProtectedMissed scans instead of trackMaxMissed) so a brief detection
    // dip can't kill the identity an aim lock is bound to. Steering already
    // refuses lost tracks, so the extra grace only affects re-identification.
    static constexpr int kProtectedMissed = 6;
    void update(const std::vector<Detection>& hits, float spawnConf,
                const DetectWatch& cfg, int64_t now, uint32_t protectId = 0);
    // Extrapolated position for display/steering. NEVER written back into
    // measBox — prediction is a rendering of the state, not a measurement.
    Rect predicted(const DetectTrack& t, int64_t now, float predS) const;
    void clear(int64_t now = 0);
    const TrackEvent* lastEvent() const { return events.empty() ? nullptr : &events.back(); }

private:
    uint32_t nextId_ = 1;
    void transition(DetectTrack& t, TrackState to, TrackReason why, int64_t now);
};

const char* trackStateName(TrackState s);
const char* trackReasonName(TrackReason r);

}  // namespace wasd
