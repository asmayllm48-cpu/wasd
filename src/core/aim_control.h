// Feedback controller for aim steering. Pure logic, testable, no IO.
//
// Inputs: the error vector from the CURRENT aim point to the reference point
// of the LATEST valid measurement. Never predicts. Output is a clamped pixel
// delta for the input engine.
//
// Guaranteed stop conditions (checked every update):
//   disabled / no lock / measurement older than maxMeasAgeUs /
//   confidence below minConf / inside deadband (with hysteresis margin).
// The deadband is a REGION, not a point: enter stop when the aim point is
// inside `enterFrac` of the box, exit only past `exitFrac` — measurement
// jitter can never flap the controller on and off.
//
// Units (fixed after the saturation audit):
//   - Error is in FRAME PIXELS per axis (not normalized units).
//   - The plant (counts -> camera) is approximated by kPxPerCount, the
//     measured frame-px of crosshair travel per emitted count. The runtime
//     calibrates it online from observed box drift; the controller only
//     consumes it. desired_px_per_s / kPxPerCount = counts_per_s.
//   - The cap is a physical SPEED (counts/s); each emit scales it by the
//     real dt so the pull rate is independent of tick frequency.
//   - ControlOut carries diagnostics (requested vs emitted rate) so the UI
//     can show how often the cap is actually binding.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/coords.h"

namespace wasd {

struct ControlCfg {
    float kp = 8.0f;               // 1/s — desired rate = errPx * kp * speedPct
    float maxRatePx = 3000.0f;     // emitted counts/s cap (physical pull speed)
    float insetX = 0.0f;           // stop zone = box inset by frac of w each side
    float insetY = 0.0f;           // stop zone = box inset by frac of h each side
    float exitFrac = 0.22f;        // resume region: stop zone grown by frac
    float deadzonePx = 1.0f;       // absolute deadband in frame px
    float maxRateHz = 200.0f;      // output rate limit
    int64_t maxMeasAgeUs = 400000; // refuse measurements older than this
    float minConf = 0.15f;         // refuse measurements below this conf
    float speedPct = 50.0f;        // user speed knob (percent)
    // Feed-forward on the MEASURED box velocity (norm units/s -> px/s of
    // screen drift). When the PLAYER moves, the box drifts at a steady screen
    // rate; pure proportional control can never close that error — it settles
    // "around" the box forever (steady-state lag). ffGain is the fraction of
    // the measured drift to match directly (1.0 = match it fully).
    float ffGain = 1.0f;
    float frameW = 1920.0f;        // video frame size: error px conversion
    float frameH = 1080.0f;
};

struct ControlOut {
    float dx = 0, dy = 0;      // counts to emit this tick (0 = hold)
    bool steering = false;     // true when an output was produced
    const char* hold = "";     // reason when not steering (diagnostics)
    float vReq = 0;            // requested rate, counts/s (pre-cap)
    float vOut = 0;            // emitted rate, counts/s (post-cap)
    bool saturated = false;    // the speed cap bound this update
};

class AimController {
public:
    bool inside = false;   // hysteresis memory: aim point inside stop region
    int64_t lastEmitUs = -100000000000LL;   // first update always emits
    // Plant gain: frame-px of crosshair travel per emitted count, measured
    // online by the runtime (box drift vs emitted counts). Clamped in use so
    // a bad estimate can never zero or explode the output.
    float kPxPerCount = 0.5f;

    void reset() { inside = false; lastEmitUs = -100000000000LL; }

    // `cur` = current aim point (screen 0..1), `box` = target box (0..1),
    // `pt` = reference point inside box, `vx,vy` = measured box velocity
    // (norm units/s) for the feed-forward. Error is converted to frame px.
    ControlOut update(Vec2 cur, const Rect& box, Vec2 pt, float vx, float vy,
                      float measConf, int64_t measUs, int64_t now,
                      const ControlCfg& c) {
        ControlOut o;
        const char* why = nullptr;
        if (measConf < c.minConf) why = "low conf";
        else if (now - measUs > c.maxMeasAgeUs) why = "stale meas";
        else if (c.maxRateHz > 0 && now - lastEmitUs < int64_t(1e6 / c.maxRateHz)) {
            o.hold = "rate"; return o;
        }
        if (why) { o.hold = why; return o; }

        // Region test with hysteresis. The STOP ZONE is the character box
        // inset by (insetX, insetY) — the aim must reach the INNER box (the
        // real hit area), not just brush the outer edge. Once inside we stay
        // stopped until the point leaves the zone expanded by exitFrac, so
        // box-edge jitter can never flap the pull on and off.
        const float ix = std::clamp(c.insetX, 0.0f, 0.45f) * box.w;
        const float iy = std::clamp(c.insetY, 0.0f, 0.45f) * box.h;
        // Hysteresis margin hugs the INNER zone only — a small fraction of the
        // box's smaller dimension.
        const float m = std::min(box.w, box.h) * c.exitFrac;
        const auto inZone = [&](float pad) {
            return cur.x >= box.x + ix - pad && cur.x <= box.x + box.w - ix + pad &&
                   cur.y >= box.y + iy - pad && cur.y <= box.y + box.h - iy + pad;
        };
        if (inside) {
            if (inZone(m)) { o.hold = "in region"; return o; }
            inside = false;
        } else if (inZone(0.0f)) {
            inside = true;
            o.hold = "reached"; return o;
        }

        // dt since the last EMIT, clamped: no catch-up burst after a hold and
        // no zero output on a fast re-emit. The rate limiter above keeps this
        // close to 1/maxRateHz in steady state.
        const float dtS = std::clamp(static_cast<float>(now - lastEmitUs) / 1e6f,
                                     0.0005f, 0.05f);

        // Error in FRAME PIXELS — one consistent unit for both axes.
        const float exPx = (pt.x - cur.x) * c.frameW;
        const float eyPx = (pt.y - cur.y) * c.frameH;
        if (std::hypot(exPx, eyPx) < c.deadzonePx) { o.hold = "deadzone"; return o; }

        // Desired screen-space speed (frame px/s) = proportional correction
        // + bounded feed-forward on the measured box drift.
        const float kp = c.kp * (c.speedPct / 100.0f);
        const float desX = exPx * kp + std::clamp(vx, -2.0f, 2.0f) * c.frameW * c.ffGain;
        const float desY = eyPx * kp + std::clamp(vy, -2.0f, 2.0f) * c.frameH * c.ffGain;

        // px/s -> counts/s through the measured plant gain, then a physical
        // speed cap (counts/s) — saturated flag tells the UI how starved we are.
        const float kEff = std::clamp(kPxPerCount, 0.02f, 20.0f);
        const float cx = desX / kEff, cy = desY / kEff;
        const float vReq = std::hypot(cx, cy);
        const float vMax = std::max(1.0f, c.maxRatePx);
        const float s = vReq > vMax ? vMax / vReq : 1.0f;
        o.vReq = vReq;
        o.vOut = vReq * s;
        o.saturated = s < 1.0f;
        o.dx = cx * s * dtS;
        o.dy = cy * s * dtS;
        lastEmitUs = now;
        o.steering = true;
        return o;
    }
};

}  // namespace wasd
