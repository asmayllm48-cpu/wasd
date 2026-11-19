#include "core/detect_tracker.h"

#include <algorithm>
#include <cmath>

namespace wasd {

namespace {
constexpr float kMatchIoU = 0.10f;
constexpr float kMaxVel = 4.0f;  // normalized units/sec clamp
constexpr size_t kMaxEvents = 128;

Vec2 ctr(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

float iou(const Rect& a, const Rect& b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    const float inter = std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 1e-6f ? inter / uni : 0.0f;
}
}  // namespace

const char* trackStateName(TrackState s) {
    switch (s) {
        case TrackState::Confirmed: return "confirmed";
        case TrackState::TempLost: return "lost";
        case TrackState::Reacquiring: return "reacquiring";
        case TrackState::Expired: return "expired";
    }
    return "?";
}

const char* trackReasonName(TrackReason r) {
    switch (r) {
        case TrackReason::Spawned: return "spawned";
        case TrackReason::Reconfirmed: return "reconfirmed";
        case TrackReason::MissedScan: return "missed scan";
        case TrackReason::ReacquireHit: return "fresh hit";
        case TrackReason::ExpiredMissed: return "expired (missed)";
        case TrackReason::ExpiredAge: return "expired (age)";
        case TrackReason::Cleared: return "cleared";
    }
    return "?";
}

void DetectTracker::transition(DetectTrack& t, TrackState to, TrackReason why, int64_t now) {
    if (t.state == to) return;
    if (events.size() >= kMaxEvents) events.pop_front();
    events.push_back({now, t.id, t.state, to, why});
    t.state = to;
    t.stateSinceUs = now;
}

Rect DetectTracker::predicted(const DetectTrack& t, int64_t now, float predS) const {
    const float dt = std::min(float(now - t.lastMeasUs) / 1e6f, predS);
    return {t.box.x + t.vx * dt, t.box.y + t.vy * dt, t.box.w, t.box.h};
}

void DetectTracker::update(const std::vector<Detection>& hits, float spawnConf,
                           const DetectWatch& cfg, int64_t now, uint32_t protectId) {
    const float kPosBlend = std::clamp(cfg.trackPosBlendPct, 10, 100) / 100.0f;
    const float kVelBlend = std::clamp(cfg.trackVelPct, 0, 100) / 100.0f;
    const float kPredictS = std::clamp(cfg.trackPredictMs, 0, 2000) / 1000.0f;
    const int kMaxMissed = std::clamp(cfg.trackMaxMissed, 0, 10);
    const int64_t kMaxAgeUs = int64_t(std::clamp(cfg.trackMaxAgeMs, 50, 5000)) * 1000;

    auto pred = [&](const DetectTrack& t) {
        const float dt = std::min(float(now - t.lastMeasUs) / 1e6f, kPredictS);
        return Rect{t.box.x + t.vx * dt, t.box.y + t.vy * dt, t.box.w, t.box.h};
    };

    // ByteTrack-style two-pass association: strong detections (>= spawnConf or
    // boosted) claim matches FIRST; only tracks left unmatched get a second
    // pass against the weak floor hits. A weak hit can keep a lost track warm
    // but can never outbid a strong detection for the same track — that is
    // what stops identity theft by blur-noise.
    auto strong = [&](const Detection& d) { return d.boosted || d.confidence >= spawnConf; };
    std::vector<char> used(hits.size(), 0);
    std::vector<char> matchedT(tracks.size(), 0);
    for (int pass = 0; pass < 2; ++pass)
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        DetectTrack& t = tracks[ti];
        if (pass == 1 && matchedT[ti]) continue;   // already claimed in pass 1
        const Rect pr = pred(t);
        float best = kMatchIoU;
        int bi = -1;
        for (size_t i = 0; i < hits.size(); ++i) {
            if (used[i] || hits[i].classId != t.classId) continue;
            if (pass == 0 && !strong(hits[i])) continue;
            const float v = iou(pr, hits[i].box);
            if (v > best) { best = v; bi = (int)i; }
        }
        if (bi < 0) {
            if (pass == 0) continue;             // retry against weak hits in pass 2
            ++t.missed;
            // A missed scan is NOT evidence for the old velocity: decay the
            // glide so stale pan estimates die within a few scans instead of
            // dragging the predicted aim point off-screen.
            t.vx *= kVelDecayPerMiss;
            t.vy *= kVelDecayPerMiss;
            if (t.state == TrackState::Confirmed || t.state == TrackState::Reacquiring)
                transition(t, TrackState::TempLost, TrackReason::MissedScan, now);
            t.reacqCount = 0;
            continue;
        }
        used[bi] = 1;
        matchedT[ti] = 1;
        const Detection& d = hits[size_t(bi)];

        // ---- lifecycle: fresh evidence ----
        const bool wasLost = t.state != TrackState::Confirmed;
        if (t.state == TrackState::TempLost) {
            transition(t, TrackState::Reacquiring, TrackReason::ReacquireHit, now);
            t.reacqCount = 1;
        } else if (t.state == TrackState::Reacquiring) {
            if (++t.reacqCount >= kReacqScans)
                transition(t, TrackState::Confirmed, TrackReason::Reconfirmed, now);
        }
        t.missed = 0;

        // ---- velocity: measured between REAL measurements only ----
        // Center-of-measBox to center-of-measBox, never blended/predicted
        // positions — a predicted glide must not feed back into v.
        const float dtm = std::max(float(now - t.lastMeasUs) / 1e6f, 0.03f);
        const Vec2 oc = ctr(t.measBox), nc = ctr(d.box);
        const float nvx = std::clamp((nc.x - oc.x) / dtm, -kMaxVel, kMaxVel);
        const float nvy = std::clamp((nc.y - oc.y) / dtm, -kMaxVel, kMaxVel);
        if (wasLost) {
            // Evidence gap ended: rebuild v from the fresh delta alone. The old
            // (pan-contaminated) velocity is discarded instead of blended in.
            t.vx = nvx * kVelBlend;
            t.vy = nvy * kVelBlend;
        } else {
            t.vx = t.vx * (1.0f - kVelBlend) + nvx * kVelBlend;
            t.vy = t.vy * (1.0f - kVelBlend) + nvy * kVelBlend;
        }

        // ---- position: smooth CENTER and SIZE independently ----
        // Blending x,y,w,h of the top-left box makes a shrinking box drag its
        // center sideways even when the target stands still; center+size keeps
        // pure size jitter out of the motion channel (and out of the aim bone
        // point, which is derived from box.y + box.h * bone).
        if (wasLost) {
            // Reacquire snaps to the fresh measurement: the old box may have
            // glided away and is no longer a valid anchor.
            t.box = d.box;
        } else {
            // Adaptive EMA: blend factor rises with measured center speed and
            // sinks when the measurement is fast-moving but weak. A standing
            // target gets heavy smoothing (jitter dies); a fast target gets
            // near-raw tracking (no visible lag). No prediction is added —
            // the displayed box can never run ahead of the last measurement
            // by more than one blend step.
            const Vec2 prc = ctr(pr), mc = nc;
            const float stepDx = mc.x - prc.x, stepDy = mc.y - prc.y;
            const float stepSpeed = std::hypot(stepDx, stepDy) / dtm;   // norm units/s
            // speed>=0.5/s saturates toward ~full measurement trust
            const float speedBoost = std::clamp(stepSpeed / 0.5f, 0.0f, 1.0f) * (1.0f - kPosBlend);
            // weak hits halve the boost so blur can't yank the box
            const float confScale = 0.5f + 0.5f * std::min(1.0f, d.confidence / std::max(spawnConf, 0.01f));
            const float blend = std::clamp(kPosBlend + speedBoost * confScale, kPosBlend, 1.0f);
            const float ncx = prc.x * (1.0f - blend) + mc.x * blend;
            const float ncy = prc.y * (1.0f - blend) + mc.y * blend;
            const float nw = t.box.w * (1.0f - blend) + d.box.w * blend;
            const float nh = t.box.h * (1.0f - blend) + d.box.h * blend;
            t.box = {ncx - nw * 0.5f, ncy - nh * 0.5f, nw, nh};
        }
        t.measBox = d.box;  // pure measurement, kept separate from `box`
        t.conf = d.confidence;
        t.boosted = d.boosted;
        t.lastMeasUs = now;
    }

    // ---- new candidates: spawned unconfirmed ----
    // A detection that survives only one scan is probably a false positive.
    // New tracks must ALSO earn kReacqScans consecutive matches before they
    // become Confirmed — transient FPs never reach the aim path at all.
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i]) continue;
        const Detection& d = hits[i];
        if (!d.boosted && d.confidence < spawnConf) continue;
        DetectTrack t;
        t.id = nextId_++;
        t.classId = d.classId;
        t.box = d.box;
        t.measBox = d.box;
        t.conf = d.confidence;
        t.boosted = d.boosted;
        t.lastMeasUs = now;
        t.stateSinceUs = now;
        t.state = TrackState::Reacquiring;
        t.reacqCount = 1;
        events.push_back({now, t.id, TrackState::Expired, TrackState::Reacquiring, TrackReason::Spawned});
        if (events.size() > kMaxEvents) events.pop_front();
        tracks.push_back(t);
    }

    // ---- expiry ----
    tracks.erase(
        std::remove_if(tracks.begin(), tracks.end(),
                       [&](DetectTrack& t) {
                           const int missLimit =
                               (t.id == protectId) ? std::max(kMaxMissed, kProtectedMissed) : kMaxMissed;
                           if (t.missed > missLimit) {
                               transition(t, TrackState::Expired, TrackReason::ExpiredMissed, now);
                               return true;
                           }
                           if (now - t.lastMeasUs > kMaxAgeUs) {
                               transition(t, TrackState::Expired, TrackReason::ExpiredAge, now);
                               return true;
                           }
                           return false;
                       }),
        tracks.end());
}

void DetectTracker::clear(int64_t now) {
    for (DetectTrack& t : tracks)
        transition(t, TrackState::Expired, TrackReason::Cleared, now);
    tracks.clear();
}

}  // namespace wasd
