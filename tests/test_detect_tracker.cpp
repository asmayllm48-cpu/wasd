// Regression tests for the detection tracker lifecycle state machine:
// Confirmed / TempLost / Reacquiring / Expired, measurement-vs-prediction
// separation, stale-velocity decay, evidence-based reacquire, center/size
// smoothing separation, and the transition log.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/automation.h"
#include "core/detect_tracker.h"

using namespace wasd;

namespace {

DetectWatch cfg() {
    DetectWatch d;
    d.confPct = 46;
    d.trackLowPct = 16;
    d.trackPosBlendPct = 44;
    d.trackVelPct = 15;
    d.trackPredictMs = 361;
    d.trackMaxMissed = 1;
    d.trackMaxAgeMs = 2421;
    return d;
}

Detection hit(float cx, float cy, float w = 0.06f, float h = 0.18f, int cls = 0, float conf = 0.7f) {
    Detection d;
    d.classId = cls;
    d.confidence = conf;
    d.box = {cx - w / 2, cy - h / 2, w, h};
    return d;
}

float cx(const Rect& r) { return r.x + r.w * 0.5f; }
float cy(const Rect& r) { return r.y + r.h * 0.5f; }
int64_t us(int ms) { return int64_t(ms) * 1000; }

const DetectWatch& C() {
    static DetectWatch d = cfg();
    return d;
}

}  // namespace

TEST_CASE("new detection spawns as unconfirmed candidate, needs evidence to confirm") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    REQUIRE(tr.tracks.size() == 1);
    CHECK(tr.tracks[0].state == TrackState::Reacquiring);
    CHECK(tr.tracks[0].reacqCount == 1);

    // Second consistent hit confirms it.
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(30));
    CHECK(tr.tracks[0].state == TrackState::Confirmed);
}

TEST_CASE("single-scan false positive never becomes confirmed and expires") {
    DetectTracker tr;
    tr.update({hit(0.6f, 0.5f)}, 0.46f, C(), us(0));    // FP appears
    CHECK(tr.tracks[0].state == TrackState::Reacquiring);
    tr.update({}, 0.46f, C(), us(30));                  // gone next scan
    CHECK(tr.tracks[0].state == TrackState::TempLost);
    tr.update({}, 0.46f, C(), us(60));                  // missed > maxMissed
    CHECK(tr.tracks.empty());
    // Logged the full lifecycle.
    bool sawLost = false, sawExpired = false;
    for (const auto& e : tr.events) {
        if (e.to == TrackState::TempLost) sawLost = true;
        if (e.to == TrackState::Expired) sawExpired = true;
    }
    CHECK(sawLost);
    CHECK(sawExpired);
}

TEST_CASE("short detection loss: TempLost, velocity decays, reacquire needs proof") {
    DetectTracker tr;
    tr.update({hit(0.4f, 0.5f, 0.10f)}, 0.46f, C(), us(0));
    tr.update({hit(0.45f, 0.5f, 0.10f)}, 0.46f, C(), us(30));  // confirmed, v learned
    REQUIRE(tr.tracks[0].state == TrackState::Confirmed);
    CHECK(std::abs(tr.tracks[0].vx) > 0.1f);            // absorbed the slide

    const float vBefore = tr.tracks[0].vx;
    tr.update({}, 0.46f, C(), us(60));                  // missed scan
    CHECK(tr.tracks[0].state == TrackState::TempLost);
    CHECK(tr.tracks[0].missed == 1);
    CHECK(std::abs(tr.tracks[0].vx) <= std::abs(vBefore) * 0.55f);  // decayed

    // One reappearing hit -> Reacquiring, NOT instantly Confirmed.
    tr.update({hit(0.48f, 0.5f, 0.10f)}, 0.46f, C(), us(90));
    CHECK(tr.tracks[0].state == TrackState::Reacquiring);
    // Second consistent hit -> Confirmed again.
    tr.update({hit(0.5f, 0.5f, 0.10f)}, 0.46f, C(), us(120));
    CHECK(tr.tracks[0].state == TrackState::Confirmed);
}

TEST_CASE("long detection loss expires the track") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(30));
    tr.update({}, 0.46f, C(), us(60));
    tr.update({}, 0.46f, C(), us(90));                  // missed=2 > maxMissed=1
    CHECK(tr.tracks.empty());
    bool expiredMissed = false;
    for (const auto& e : tr.events)
        if (e.why == TrackReason::ExpiredMissed) expiredMissed = true;
    CHECK(expiredMissed);
}

TEST_CASE("measurement is never polluted by prediction (measBox separate)") {
    DetectTracker tr;
    tr.update({hit(0.3f, 0.5f, 0.12f)}, 0.46f, C(), us(0));
    tr.update({hit(0.36f, 0.5f, 0.12f)}, 0.46f, C(), us(30));   // moving -> v>0
    REQUIRE(tr.tracks[0].vx > 0.01f);
    const Rect m0 = tr.tracks[0].measBox;
    // Glide the predicted box far right via prediction — measBox must stay put.
    const Rect pr = tr.predicted(tr.tracks[0], us(400), 0.35f);
    CHECK(cx(pr) > cx(m0));                              // prediction leads
    tr.update({}, 0.46f, C(), us(400));                  // one missed scan
    CHECK(cx(tr.tracks[0].measBox) == doctest::Approx(cx(m0)));  // untouched
}

TEST_CASE("velocity after reacquire is rebuilt, not blended with stale glide") {
    DetectTracker tr;
    tr.update({hit(0.3f, 0.5f, 0.12f)}, 0.46f, C(), us(0));
    tr.update({hit(0.36f, 0.5f, 0.12f)}, 0.46f, C(), us(30));   // pan velocity learned
    const float vPan = tr.tracks[0].vx;
    tr.update({}, 0.46f, C(), us(60));                  // miss -> TempLost
    tr.update({hit(0.36f, 0.5f, 0.12f)}, 0.46f, C(), us(90));   // same spot: real v ~ 0
    // Old code: v kept most of the stale pan velocity. New: rebuilt from the
    // fresh delta — near zero for a static target.
    CHECK(std::abs(tr.tracks[0].vx) < std::abs(vPan) * 0.5f);
}

TEST_CASE("pure size change does not move the smoothed center") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f, 0.06f, 0.18f)}, 0.46f, C(), us(0));
    tr.update({hit(0.5f, 0.5f, 0.06f, 0.18f)}, 0.46f, C(), us(30));
    const float c0 = cx(tr.tracks[0].box);
    // Same center, smaller box (partial occlusion): center must stay still.
    tr.update({hit(0.5f, 0.5f, 0.05f, 0.14f)}, 0.46f, C(), us(60));
    CHECK(cx(tr.tracks[0].box) == doctest::Approx(c0).epsilon(0.01));
    CHECK(std::abs(tr.tracks[0].vx) < 0.05f);           // no phantom velocity
}

TEST_CASE("two close objects keep separate identities") {
    DetectTracker tr;
    tr.update({hit(0.4f, 0.5f), hit(0.52f, 0.5f)}, 0.46f, C(), us(0));
    tr.update({hit(0.4f, 0.5f), hit(0.52f, 0.5f)}, 0.46f, C(), us(30));
    REQUIRE(tr.tracks.size() == 2);
    CHECK(tr.tracks[0].id != tr.tracks[1].id);
    // Drift the second one a bit — identities must not swap.
    tr.update({hit(0.4f, 0.5f), hit(0.56f, 0.5f)}, 0.46f, C(), us(60));
    CHECK(cx(tr.tracks[1].measBox) == doctest::Approx(0.56f).epsilon(0.05));
}

TEST_CASE("identity switch cannot hijack a track without IoU evidence") {
    DetectTracker tr;
    tr.update({hit(0.4f, 0.5f)}, 0.46f, C(), us(0));
    tr.update({hit(0.4f, 0.5f)}, 0.46f, C(), us(30));
    const uint32_t id0 = tr.tracks[0].id;
    // A different object pops far away — must NOT re-match the existing track.
    tr.update({hit(0.85f, 0.3f)}, 0.46f, C(), us(60));
    REQUIRE(tr.tracks.size() == 2);
    CHECK(tr.tracks[0].id == id0);
    CHECK(tr.tracks[0].state == TrackState::TempLost);  // original lost, not moved
    CHECK(tr.tracks[1].state == TrackState::Reacquiring);
}

TEST_CASE("transition log records every state change with its reason") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(30));
    tr.update({}, 0.46f, C(), us(60));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(90));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(120));
    std::vector<TrackReason> whys;
    for (const auto& e : tr.events) whys.push_back(e.why);
    CHECK(std::find(whys.begin(), whys.end(), TrackReason::Spawned) != whys.end());
    CHECK(std::find(whys.begin(), whys.end(), TrackReason::Reconfirmed) != whys.end());
    CHECK(std::find(whys.begin(), whys.end(), TrackReason::MissedScan) != whys.end());
    CHECK(std::find(whys.begin(), whys.end(), TrackReason::ReacquireHit) != whys.end());
    // from->to pairs are coherent.
    for (const auto& e : tr.events) CHECK(e.from != e.to);
}

TEST_CASE("age expiry kills stale tracks that keep no fresh evidence") {
    DetectTracker tr;
    DetectWatch d = C();
    d.trackMaxAgeMs = 200;  // short for the test
    tr.update({hit(0.5f, 0.5f)}, 0.46f, d, us(0));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, d, us(30));
    // Keep missing but stay under maxMissed is impossible; instead age out:
    // hits keep arriving but the track is gone — simulate pure time passing.
    tr.update({}, 0.46f, d, us(60));   // lost
    tr.update({}, 0.46f, d, us(300));  // way past maxAge AND past maxMissed
    CHECK(tr.tracks.empty());
}

TEST_CASE("protected (locked) track survives extra misses before expiring") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(30));
    REQUIRE(tr.tracks[0].state == TrackState::Confirmed);
    const uint32_t id = tr.tracks[0].id;
    // 3 missed scans: a normal track dies at missed>1, the protected one stays.
    tr.update({}, 0.46f, C(), us(60), id);
    tr.update({}, 0.46f, C(), us(90), id);
    tr.update({}, 0.46f, C(), us(120), id);
    REQUIRE(tr.tracks.size() == 1);
    CHECK(tr.tracks[0].state == TrackState::TempLost);
    // Unprotected same track would already be gone:
    DetectTracker tr2;
    tr2.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    tr2.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(30));
    tr2.update({}, 0.46f, C(), us(60));
    tr2.update({}, 0.46f, C(), us(90));
    CHECK(tr2.tracks.empty());
    // Beyond the protected budget it expires too:
    for (int ms = 150; ms <= 400; ms += 30) tr.update({}, 0.46f, C(), us(ms), id);
    CHECK(tr.tracks.empty());
}

TEST_CASE("clear() expires all tracks and empties the list") {
    DetectTracker tr;
    tr.update({hit(0.5f, 0.5f)}, 0.46f, C(), us(0));
    tr.clear(us(10));
    CHECK(tr.tracks.empty());
    bool cleared = false;
    for (const auto& e : tr.events)
        if (e.why == TrackReason::Cleared) cleared = true;
    CHECK(cleared);
}
