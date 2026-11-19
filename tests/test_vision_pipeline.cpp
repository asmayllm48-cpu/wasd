// Vision pipeline v2: fusion, frame scheduling, target selection/lock FSM,
// controller limits. All pure logic — no ONNX, no GPU, no threads.
#include <doctest/doctest.h>

#include "core/aim_control.h"
#include "core/target_manager.h"
#include "core/vision_frame.h"
#include "core/vision_fusion.h"

using namespace wasd;

static Detection det(float x, float y, float w, float h, float c, int cls = 0) {
    Detection d;
    d.box = {x, y, w, h};
    d.confidence = c;
    d.classId = cls;
    return d;
}

// ============================== Detection Fusion ===========================
TEST_CASE("fusion merges a part box contained inside a character box") {
    std::vector<Detection> hits = {
        det(0.40f, 0.20f, 0.10f, 0.30f, 0.80f),   // body
        det(0.42f, 0.21f, 0.05f, 0.08f, 0.60f),   // face inside body
    };
    auto out = fuseDetections(hits, FusionCfg{});
    CHECK(out.size() == 1);
    CHECK(out[0].fused);
    CHECK(out[0].parts.size() == 2);   // originals preserved
}

TEST_CASE("fusion keeps two separate players as two records") {
    std::vector<Detection> hits = {
        det(0.10f, 0.30f, 0.10f, 0.30f, 0.80f),
        det(0.70f, 0.30f, 0.10f, 0.30f, 0.75f),
    };
    auto out = fuseDetections(hits, FusionCfg{});
    CHECK(out.size() == 2);
}

TEST_CASE("fusion never merges merely-overlapping different-class boxes") {
    std::vector<Detection> hits = {
        det(0.40f, 0.20f, 0.10f, 0.30f, 0.80f, 0),   // enemy
        det(0.42f, 0.22f, 0.08f, 0.25f, 0.70f, 1),   // self, overlapping
    };
    auto out = fuseDetections(hits, FusionCfg{});
    CHECK(out.size() == 2);   // no compat entry -> kept apart
}

TEST_CASE("fusion merges compatible classes only when configured") {
    std::vector<Detection> hits = {
        det(0.40f, 0.20f, 0.10f, 0.30f, 0.80f, 0),
        det(0.42f, 0.22f, 0.08f, 0.25f, 0.70f, 2),
    };
    auto out = fuseDetections(hits, FusionCfg{}, {{0, 2}});
    CHECK(out.size() == 1);
}

TEST_CASE("fusion marks boxes clipped by the frame edge as partial") {
    auto out = fuseDetections({det(0.0f, 0.30f, 0.10f, 0.30f, 0.8f)}, FusionCfg{});
    CHECK(out[0].partial);
}

// ============================== InferScheduler =============================
TEST_CASE("fixed rate limits launch cadence") {
    InferScheduler s;
    s.setMode(30);                       // ~33.3ms period
    CHECK(s.due(0, false));
    CHECK_FALSE(s.due(10000, false));    // 10ms later: too soon
    CHECK(s.due(40000, false));
    CHECK(s.skippedByRate() == 1);
}

TEST_CASE("busy worker never gets a second job") {
    InferScheduler s;
    s.setMode(60);
    CHECK(s.due(0, false));
    CHECK_FALSE(s.due(20000, true));     // still busy -> no pile-up
}

TEST_CASE("auto mode adapts rate to measured p95") {
    InferScheduler s;
    s.setMode(0);                        // Auto
    int64_t t = 0;
    for (int i = 0; i < 40; ++i) { s.noteInferUs(30000, t); t += 2000000; }
    CHECK(s.effectiveFps() <= 20);       // 30ms infer -> ~15-20 fps budget
    CHECK(s.effectiveFps() >= 15);
}

TEST_CASE("mailbox keeps only the latest frame and counts skips") {
    LatestMailbox<int> mb;
    mb.push(1); mb.push(2); mb.push(3);
    int v = 0;
    CHECK(mb.take(v));
    CHECK(v == 3);                       // never stale
    CHECK(mb.skipped() == 2);
    CHECK_FALSE(mb.take(v));
}

// ============================== Target selection ===========================
static TargetCandidate cand(uint32_t id, float x, float y, float w, float h, float c) {
    TargetCandidate t;
    t.trackId = id;
    t.box = {x, y, w, h};
    t.conf = c;
    return t;
}

TEST_CASE("center preset picks the candidate nearest the anchor") {
    auto cands = {cand(1, 0.45f, 0.45f, 0.1f, 0.2f, 0.5f),   // near center, weak
                  cand(2, 0.80f, 0.80f, 0.1f, 0.2f, 0.95f)}; // far, strong
    CHECK(pickTarget({cands.begin(), cands.end()}, {0.5f, 0.5f}, selectPreset(0)) == 1);
}

TEST_CASE("size preset picks the biggest box") {
    auto cands = {cand(1, 0.45f, 0.45f, 0.05f, 0.1f, 0.9f),
                  cand(2, 0.70f, 0.70f, 0.20f, 0.4f, 0.5f)};
    CHECK(pickTarget({cands.begin(), cands.end()}, {0.5f, 0.5f}, selectPreset(1)) == 2);
}

TEST_CASE("unconfirmed candidates are never picked") {
    auto c = cand(1, 0.45f, 0.45f, 0.1f, 0.2f, 0.9f);
    c.confirmed = false;
    CHECK(pickTarget({c}, {0.5f, 0.5f}, selectPreset(0)) == 0);
}

// ============================== Lock state machine =========================
TEST_CASE("lock holds its identity while a closer challenger appears") {
    TargetLock L;
    LockCfg lc;
    auto sw = selectPreset(0);
    // frame 1: only target 1 visible -> lock
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)};
    CHECK(L.feed(f1, {0.5f, 0.5f}, sw, lc, 0) != nullptr);
    CHECK(L.trackId == 1);
    // frame 2: target 2 is now nearer the center — identity must NOT switch
    std::vector<TargetCandidate> f2 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f),
                                       cand(2, 0.49f, 0.49f, 0.1f, 0.2f, 0.9f)};
    const TargetCandidate* s = L.feed(f2, {0.5f, 0.5f}, sw, lc, 30000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 1);
}

TEST_CASE("temp-lost pauses steering but keeps identity until grace expires") {
    TargetLock L;
    LockCfg lc; lc.graceUs = 150000;
    auto sw = selectPreset(0);
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f),
                                       cand(2, 0.49f, 0.49f, 0.1f, 0.2f, 0.9f)};
    L.feed(f1, {0.5f, 0.5f}, sw, lc, 0);
    // target 1 vanishes for 100ms -> pause, keep identity, DON'T take target 2
    std::vector<TargetCandidate> f2 = {cand(2, 0.49f, 0.49f, 0.1f, 0.2f, 0.9f)};
    CHECK(L.feed(f2, {0.5f, 0.5f}, sw, lc, 100000) == nullptr);
    CHECK(L.state == LockState::TempLost);
    CHECK(L.trackId == 1);
    // back before grace -> re-lock on the same identity
    std::vector<TargetCandidate> f3 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f),
                                       cand(2, 0.49f, 0.49f, 0.1f, 0.2f, 0.9f)};
    const TargetCandidate* s = L.feed(f3, {0.5f, 0.5f}, sw, lc, 120000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 1);
    CHECK(L.state == LockState::Locked);
}

TEST_CASE("grace expiry selects a new target") {
    TargetLock L;
    LockCfg lc; lc.graceUs = 100000;
    auto sw = selectPreset(0);
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)};
    L.feed(f1, {0.5f, 0.5f}, sw, lc, 0);
    std::vector<TargetCandidate> f2 = {cand(2, 0.49f, 0.49f, 0.1f, 0.2f, 0.9f)};
    L.feed(f2, {0.5f, 0.5f}, sw, lc, 50000);       // -> TempLost
    L.feed(f2, {0.5f, 0.5f}, sw, lc, 200000);      // grace expired -> SelectingNew
    const TargetCandidate* s = L.feed(f2, {0.5f, 0.5f}, sw, lc, 210000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 2);                        // new identity chosen
}

TEST_CASE("a new id near the last lock point re-links as continuity") {
    TargetLock L;
    LockCfg lc; lc.graceUs = 150000; lc.continuityRadius = 0.15f;
    auto sw = selectPreset(0);
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f),
                                       cand(7, 0.80f, 0.80f, 0.1f, 0.2f, 0.9f)};
    L.feed(f1, {0.5f, 0.5f}, sw, lc, 0);
    // track 1 lost; a NEW track 9 appears near the old spot -> continuity
    std::vector<TargetCandidate> f2 = {cand(9, 0.47f, 0.46f, 0.1f, 0.2f, 0.7f),
                                       cand(7, 0.80f, 0.80f, 0.1f, 0.2f, 0.9f)};
    L.feed(f2, {0.5f, 0.5f}, sw, lc, 60000);          // Locked -> TempLost
    const TargetCandidate* s = L.feed(f2, {0.5f, 0.5f}, sw, lc, 90000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 9);   // adopted, not the farther track 7
}

TEST_CASE("cancel drops the identity") {
    TargetLock L;
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)};
    L.feed(f1, {0.5f, 0.5f}, selectPreset(0), LockCfg{}, 0);
    L.cancel(1000);
    CHECK(L.trackId == 0);
    CHECK(L.state == LockState::Cancelled);
}

TEST_CASE("internal release re-acquires with a cooldown on the released id") {
    TargetLock L;
    LockCfg lc;
    auto sw = selectPreset(0);
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f),
                                       cand(2, 0.55f, 0.55f, 0.1f, 0.2f, 0.9f)};
    L.feed(f1, {0.5f, 0.5f}, sw, lc, 0);
    L.cancel(1000);   // internal release (divergence / off-frame escape)
    // key still held -> must re-acquire; released id 1 is on cooldown -> id 2
    const TargetCandidate* s = L.feed(f1, {0.5f, 0.5f}, sw, lc, 100000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 2);
    // after the cooldown window the released id may be picked again
    TargetLock L2;
    L2.feed(std::vector<TargetCandidate>{cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)},
            {0.5f, 0.5f}, sw, lc, 0);
    L2.cancel(1000);
    s = L2.feed(std::vector<TargetCandidate>{cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)},
                {0.5f, 0.5f}, sw, lc, 700000);
    REQUIRE(s != nullptr);
    CHECK(s->trackId == 1);
}

TEST_CASE("lock transitions are logged with reasons") {
    TargetLock L;
    LockCfg lc; lc.graceUs = 50000;
    std::vector<TargetCandidate> f1 = {cand(1, 0.46f, 0.45f, 0.1f, 0.2f, 0.8f)};
    L.feed(f1, {0.5f, 0.5f}, selectPreset(0), lc, 0);
    L.feed({}, {0.5f, 0.5f}, selectPreset(0), lc, 10000);   // -> TempLost
    L.feed({}, {0.5f, 0.5f}, selectPreset(0), lc, 90000);   // -> expired
    REQUIRE(L.log.size() >= 3);
    CHECK(L.log[0].to == LockState::Locked);
    CHECK(L.log[1].to == LockState::TempLost);
    CHECK(L.log.back().to == LockState::SelectingNew);
}

// ============================== Target point ===============================
TEST_CASE("point presets land inside the box and scale with it") {
    Rect b{0.4f, 0.2f, 0.1f, 0.4f};
    CHECK(pointInBox(b, PointPreset::Center, 0.5f, 0.5f).y == doctest::Approx(0.4f));
    CHECK(pointInBox(b, PointPreset::Head, 0.5f, 0.5f).y == doctest::Approx(0.26f));
    CHECK(pointInBox(b, PointPreset::Custom, 0.2f, 0.9f).x == doctest::Approx(0.42f));
}

// ============================== Controller =================================
TEST_CASE("controller stops inside the box and resumes past the margin") {
    AimController c;
    ControlCfg cc; cc.speedPct = 50; cc.maxRateHz = 0;
    Rect box{0.45f, 0.40f, 0.10f, 0.20f};
    // outside -> steer
    ControlOut o = c.update({0.5f, 0.70f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc);
    CHECK(o.steering);
    // inside box -> stop, and STAY stopped inside the margin
    o = c.update({0.5f, 0.55f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 1000, cc);
    CHECK_FALSE(o.steering);
    CHECK(c.inside);
    o = c.update({0.5f, 0.62f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 2000, cc);  // in margin
    CHECK_FALSE(o.steering);
    // beyond the exit margin -> resume
    o = c.update({0.5f, 0.75f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 3000, cc);
    CHECK(o.steering);
}

TEST_CASE("inner stop zone: edge is not enough, center region stops the pull") {
    AimController c;
    ControlCfg cc; cc.speedPct = 50; cc.maxRateHz = 0;
    cc.insetX = cc.insetY = 0.25f;   // stop zone = middle 50% of the box
    Rect box{0.40f, 0.30f, 0.20f, 0.40f};
    // inside the OUTER box but outside the inner zone -> keep steering
    CHECK(c.update({0.42f, 0.50f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc).steering);
    // inside the inner zone -> stop
    CHECK_FALSE(c.update({0.50f, 0.50f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 1000, cc).steering);
    CHECK(c.inside);
    // back outside zone+margin -> resume
    CHECK(c.update({0.5f, 0.85f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 2000, cc).steering);
}

TEST_CASE("parked inside outer box but outside inner zone must still steer") {
    AimController c;
    ControlCfg cc; cc.speedPct = 50; cc.maxRateHz = 0; cc.insetX = cc.insetY = 0.25f;
    Rect box{0.40f, 0.30f, 0.20f, 0.40f};   // inner zone y = [0.40, 0.60]
    // inside the OUTER box but below the inner zone -> steer, never latch
    CHECK(c.update({0.5f, 0.65f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc).steering);
    // latch once inside the inner zone
    CHECK_FALSE(c.update({0.5f, 0.50f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 1000, cc).steering);
    // drift out of the inner zone past the (now small) margin -> resume.
    // The old 22%-of-box margin covered the whole outer box — the pull could
    // never resume; this was the "stuck inside the big box" bug.
    CHECK(c.update({0.5f, 0.70f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 2000, cc).steering);
}

TEST_CASE("controller refuses stale and weak measurements") {
    AimController c;
    ControlCfg cc; cc.maxMeasAgeUs = 100000; cc.minConf = 0.3f; cc.maxRateHz = 0;
    Rect box{0.45f, 0.40f, 0.10f, 0.20f};
    CHECK_FALSE(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 200000, cc).steering); // stale
    CHECK_FALSE(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.1f, 0, 0, cc).steering);      // weak
    CHECK(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc).steering);
}

TEST_CASE("controller output is capped by physical speed") {
    AimController c;
    ControlCfg cc; cc.speedPct = 100; cc.maxRatePx = 50; cc.maxRateHz = 0;
    Rect box{0.45f, 0.40f, 0.10f, 0.20f};
    // First emit: dt clamps to 50ms -> the cap of 50 counts/s emits <= 2.5.
    ControlOut o = c.update({0.5f, 0.95f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc);
    CHECK(o.steering);
    CHECK(o.saturated);                       // huge error, tiny cap -> binding
    CHECK(o.vOut == doctest::Approx(50.0f));
    CHECK(std::hypot(o.dx, o.dy) <= 50.0f * 0.05f + 1e-4);
}

TEST_CASE("controller output scales with dt, not with call rate") {
    AimController a, b;
    ControlCfg cc; cc.speedPct = 100; cc.maxRatePx = 1e9f; cc.maxRateHz = 0;
    Rect box{0.45f, 0.40f, 0.10f, 0.20f};
    ControlOut o1 = a.update({0.5f, 0.95f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc);
    b.lastEmitUs = -5000;                     // 5ms gap instead of the 50ms clamp
    ControlOut o2 = b.update({0.5f, 0.95f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc);
    // dt 50ms vs 5ms -> the longer-dt emit is 10x the shorter one.
    CHECK(o1.dy == doctest::Approx(o2.dy * 10.0f).epsilon(0.01));
}

TEST_CASE("feed-forward tracks a drifting target") {
    ControlCfg cc; cc.speedPct = 50; cc.maxRateHz = 0; cc.ffGain = 1.0f;
    Rect box{0.40f, 0.30f, 0.20f, 0.40f};
    // crosshair outside the box, box drifting right at 0.4 norm/s:
    // the feed-forward must add to the proportional pull (no steady lag).
    AimController a, b;
    ControlOut off = a.update({0.30f, 0.50f}, box, {0.5f, 0.45f},
                             0.0f, 0.0f, 0.9f, 0, 0, cc);
    ControlOut on  = b.update({0.30f, 0.50f}, box, {0.5f, 0.45f},
                             0.4f, 0.0f, 0.9f, 0, 0, cc);
    CHECK(on.dx > off.dx);
}

TEST_CASE("rate limiter blocks rapid re-emits") {
    AimController c;
    ControlCfg cc; cc.maxRateHz = 50;   // 20ms
    Rect box{0.45f, 0.40f, 0.10f, 0.20f};
    CHECK(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 0, cc).steering);
    CHECK_FALSE(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 5000, cc).steering);
    CHECK(c.update({0.5f, 0.8f}, box, {0.5f, 0.45f}, 0.0f, 0.0f, 0.9f, 0, 25000, cc).steering);
}
