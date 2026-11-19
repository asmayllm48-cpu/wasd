#include "core/target_manager.h"

#include <algorithm>
#include <cmath>

namespace wasd {

const char* lockStateName(LockState s) {
    switch (s) {
        case LockState::Idle: return "idle";
        case LockState::Acquiring: return "acquiring";
        case LockState::Locked: return "locked";
        case LockState::TempLost: return "temp-lost";
        case LockState::Reacquiring: return "reacquiring";
        case LockState::Cancelled: return "cancelled";
        case LockState::SelectingNew: return "selecting";
    }
    return "?";
}

const char* lockEventName(LockEvent e) {
    switch (e) {
        case LockEvent::Acquired: return "acquired";
        case LockEvent::SteerPause: return "steer paused";
        case LockEvent::FreshEvidence: return "fresh evidence";
        case LockEvent::ReLocked: return "re-locked";
        case LockEvent::LostGraceExpired: return "grace expired";
        case LockEvent::TrackExpired: return "track expired";
        case LockEvent::CancelledByUser: return "cancelled";
        case LockEvent::NewTargetChosen: return "new target";
    }
    return "?";
}

SelectWeights selectPreset(int preset) {
    switch (preset) {
        case 1: return {0.15f, 1.0f, 0.1f, 0.0f};   // Size priority
        case 2: return {0.15f, 0.0f, 1.0f, 0.0f};   // Confidence priority
        case 3: return {0.6f, 0.25f, 0.35f, 0.0f};  // Balanced
        case 0:
        default: return {1.0f, 0.0f, 0.25f, 0.0f};  // Center priority
    }
}

const char* selectPresetName(int preset) {
    switch (preset) {
        case 1: return "Size priority";
        case 2: return "Confidence priority";
        case 3: return "Balanced";
        default: return "Center priority";
    }
}

static float score(const TargetCandidate& c, Vec2 anchor, const SelectWeights& w,
                   float classBonus, int preferClass) {
    const Vec2 cc{c.box.x + c.box.w * 0.5f, c.box.y + c.box.h * 0.5f};
    // screen-space distance, normalized by the frame diagonal (==1.0 norm)
    const float dc = std::hypot(cc.x - anchor.x, cc.y - anchor.y);
    const float prox = 1.0f - std::clamp(dc / 0.7071f, 0.0f, 1.0f);   // 0..1
    const float size = std::clamp(std::sqrt(c.box.w * c.box.h), 0.0f, 1.0f);
    float s = w.center * prox + w.size * size + w.conf * c.conf;
    if (preferClass >= 0) s += w.priority * classBonus;   // applied by caller only when class matches
    return s;
}

uint32_t pickTarget(const std::vector<TargetCandidate>& cands, Vec2 anchor,
                    const SelectWeights& w, float classBonus, int preferClass) {
    float best = -1e9f;
    uint32_t id = 0;
    for (const auto& c : cands) {
        if (!c.confirmed) continue;
        float s = score(c, anchor, w, classBonus, preferClass);
        if (s > best) { best = s; id = c.trackId; }
    }
    return id;
}

const char* pointPresetName(int p) {
    switch (p) {
        case 1: return "Head";
        case 2: return "Chest";
        case 3: return "Upper body";
        case 4: return "Custom";
        default: return "Center";
    }
}

Vec2 pointInBox(const Rect& b, PointPreset p, float relX, float relY) {
    // Presets are (relX, relY) inside the character box so the point tracks
    // the box when its size changes.
    float rx = 0.5f, ry = 0.5f;
    switch (p) {
        case PointPreset::Head:      ry = 0.15f; break;
        case PointPreset::Chest:     ry = 0.32f; break;
        case PointPreset::UpperBody: ry = 0.42f; break;
        case PointPreset::Custom:    rx = relX; ry = relY; break;
        case PointPreset::Center:
        default: break;
    }
    return {b.x + b.w * std::clamp(rx, 0.0f, 1.0f), b.y + b.h * std::clamp(ry, 0.0f, 1.0f)};
}

void TargetLock::go(LockState to, LockEvent why, int64_t now, uint32_t tid) {
    if (log.size() >= 64) log.pop_front();
    log.push_back({now, state, to, why, tid ? tid : trackId});
    state = to;
    stateSinceUs = now;
}

void TargetLock::reset(int64_t now) {
    if (state != LockState::Idle) go(LockState::Idle, LockEvent::CancelledByUser, now);
    trackId = 0;
}

void TargetLock::acquire(const TargetCandidate& c, Vec2 pt, float score, int64_t now) {
    trackId = c.trackId;
    lastBox = c.box;
    lastPoint = pt;
    scoreAtLock = score;
    lastEvidenceUs = now;
    go(LockState::Locked, LockEvent::Acquired, now);
}

void TargetLock::cancel(int64_t now) {
    releasedId = trackId;
    releasedAtUs = now;
    go(LockState::Cancelled, LockEvent::CancelledByUser, now);
    trackId = 0;
}

const TargetCandidate* TargetLock::feed(const std::vector<TargetCandidate>& cands,
                                        Vec2 anchor, const SelectWeights& w,
                                        const LockCfg& cfg, int64_t now) {
    const TargetCandidate* mine = nullptr;
    for (const auto& c : cands)
        if (c.trackId == trackId && c.confirmed) { mine = &c; break; }

    switch (state) {
        case LockState::Locked:
            if (mine) {
                lastBox = mine->box;
                lastEvidenceUs = now;
                return mine;                       // keep steering this identity
            }
            // fresh scan without our identity -> pause steering, keep identity
            go(LockState::TempLost, LockEvent::SteerPause, now);
            return nullptr;

        case LockState::TempLost:
            if (mine) {
                lastBox = mine->box;
                lastEvidenceUs = now;
                go(LockState::Locked, LockEvent::ReLocked, now);
                return mine;
            }
            // continuity: a NEW confirmed id near our last box counts as same
            // object returning — adopt it before the grace window closes.
            for (const auto& c : cands) {
                if (!c.confirmed) continue;
                const Vec2 cc{c.box.x + c.box.w * .5f, c.box.y + c.box.h * .5f};
                const Vec2 oc{lastBox.x + lastBox.w * .5f, lastBox.y + lastBox.h * .5f};
                if (std::hypot(cc.x - oc.x, cc.y - oc.y) <= cfg.continuityRadius) {
                    trackId = c.trackId;
                    lastBox = c.box;
                    lastEvidenceUs = now;
                    go(LockState::Locked, LockEvent::ReLocked, now);
                    return &c;
                }
            }
            if (now - stateSinceUs > cfg.graceUs) {
                go(LockState::SelectingNew, LockEvent::LostGraceExpired, now);
                trackId = 0;
            }
            return nullptr;

        case LockState::Cancelled:
            // Not terminal: an internal release (divergence guard, off-frame,
            // hand-swing escape) must NOT kill re-acquisition while the key is
            // still held — fall through and select again. `reset()` (key
            // release) is the only real stop, and it lands on Idle anyway.
            state = LockState::SelectingNew;
            [[fallthrough]];

        case LockState::Idle:
        case LockState::Acquiring:
        case LockState::SelectingNew: {
            // Fresh selection: prefer a candidate near the remembered lock
            // point (spatial continuity) within grace memory, else best score.
            // A just-released id gets a short cooldown so a pathological lock
            // can't instantly re-grab the same bad target in a loop.
            const bool cooling = releasedId && now - releasedAtUs < 500000;
            const TargetCandidate* near = nullptr;
            for (const auto& c : cands) {
                if (!c.confirmed || (cooling && c.trackId == releasedId)) continue;
                const Vec2 cc{c.box.x + c.box.w * .5f, c.box.y + c.box.h * .5f};
                if (std::hypot(cc.x - lastPoint.x, cc.y - lastPoint.y) <= cfg.continuityRadius)
                    { near = &c; break; }
            }
            const TargetCandidate* chosen = near;
            float sc = 0;
            if (!chosen) {
                std::vector<TargetCandidate> pool;
                if (cooling) {
                    for (const auto& c : cands)
                        if (c.trackId != releasedId) pool.push_back(c);
                } else pool = cands;
                const uint32_t id = pickTarget(pool, anchor, w);
                if (id)
                    for (const auto& c : cands) if (c.trackId == id) { chosen = &c; break; }
            }
            if (chosen) {
                sc = score(*chosen, anchor, w, 0, -1);
                acquire(*chosen, anchor, sc, now);
                if (!log.empty()) log.back().why = LockEvent::NewTargetChosen;
                return chosen;
            }
            state = LockState::Idle;
            return nullptr;
        }

        case LockState::Reacquiring:
            return nullptr;
    }
    return nullptr;
}

}  // namespace wasd
