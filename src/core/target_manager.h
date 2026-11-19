// Target Manager: selection scoring, lock state machine, and reference point.
// Pure logic, fully testable — owns NO threads, NO IO.
//
// Selection: weighted score per candidate (screen-space center proximity, size,
// confidence, class priority) with presets; the switch from the current pick
// to a challenger requires score >= curScore * (1 + hysteresis) so ties don't
// flap.
//
// Lock FSM (explicit, logged):
//   Idle -> Acquiring -> Locked -> TempLost -> Reacquiring -> Locked
//                    \-> LockCancelled / Expired -> SelectingNew -> Locked
// Rules enforced: while Locked the lock belongs to the IDENTITY — a "better"
// challenger can never steal it. While TempLost steering must not fire (no
// stale data). Re-entry happens only through fresh Confirmed evidence on the
// same track id (or a fresh track near the remembered lock point within the
// continuity radius).
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/coords.h"
#include "core/detect_tracker.h"

namespace wasd {

enum class LockState : uint8_t {
    Idle, Acquiring, Locked, TempLost, Reacquiring, Cancelled, SelectingNew
};

enum class LockEvent : uint8_t {
    Acquired, SteerPause, FreshEvidence, ReLocked, LostGraceExpired,
    TrackExpired, CancelledByUser, NewTargetChosen
};

const char* lockStateName(LockState s);
const char* lockEventName(LockEvent e);

struct LockRecord {
    int64_t tUs = 0;
    LockState from = LockState::Idle, to = LockState::Idle;
    LockEvent why = LockEvent::Acquired;
    uint32_t trackId = 0;
};

// ---- selection weights ----------------------------------------------------
struct SelectWeights {
    float center = 1.0f;   // proximity to aim/center point (screen-space!)
    float size = 0.0f;     // larger box
    float conf = 0.25f;    // higher confidence
    float priority = 0.0f; // class-priority multiplier (see classBonus)
};
// presets: 0 Center, 1 Size, 2 Confidence, 3 Balanced
SelectWeights selectPreset(int preset);
const char* selectPresetName(int preset);

struct TargetCandidate {
    uint32_t trackId = 0;
    Rect box{};
    float conf = 0;
    bool confirmed = true;   // TrackState::Confirmed only
    TrackState state = TrackState::Confirmed;
};

// One evaluation pass: returns best candidate id (0 = none). `anchor` is the
// screen-space reference point (aim center or custom point).
uint32_t pickTarget(const std::vector<TargetCandidate>& cands, Vec2 anchor,
                    const SelectWeights& w, float classBonus = 0.0f,
                    int preferClass = -1);

// ---- reference point inside the character box ------------------------------
// Presets express a (relX, relY) INSIDE the box so the point scales with box
// size; Custom uses user relX/relY.
enum class PointPreset : uint8_t { Center = 0, Head, Chest, UpperBody, Custom };
const char* pointPresetName(int p);
Vec2 pointInBox(const Rect& box, PointPreset p, float relX, float relY);

// ---- lock state machine ----------------------------------------------------
struct LockCfg {
    int64_t graceUs = 150000;      // how long TempLost survives before expiry
    float continuityRadius = 0.15f;// norm radius to re-link a NEW id near last lock
    float switchHysteresis = 0.15f;// challenger must beat score by this fraction
};

class TargetLock {
public:
    LockState state = LockState::Idle;
    uint32_t trackId = 0;          // owned identity (survives TempLost)
    Rect lastBox{};                // last confirmed box under lock
    Vec2 lastPoint{};              // last computed aim point
    float scoreAtLock = 0;
    int64_t stateSinceUs = 0;
    int64_t lastEvidenceUs = 0;
    uint32_t releasedId = 0;       // id released by cancel() — cooldown before re-pick
    int64_t releasedAtUs = 0;
    std::deque<LockRecord> log;    // bounded 64

    void reset(int64_t now);
    void acquire(const TargetCandidate& c, Vec2 pt, float score, int64_t now);
    void cancel(int64_t now);
    // Feed every scan's CONFIRMED candidates. Returns the candidate to steer
    // (nullptr = do not steer this tick).
    const TargetCandidate* feed(const std::vector<TargetCandidate>& cands,
                                Vec2 anchor, const SelectWeights& w,
                                const LockCfg& cfg, int64_t now);

private:
    void go(LockState to, LockEvent why, int64_t now, uint32_t tid = 0);
};

}  // namespace wasd
