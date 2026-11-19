#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "core/input_engine.h"

using namespace wasd;
using proto::MotionAction;

namespace {
struct Rec {
    std::vector<proto::TouchEvent> ev;
    InputEngine engine{[this](const proto::TouchEvent& e) { ev.push_back(e); }};
    Rec() {
        engine.setFrameSize(2000, 1000);
        engine.setProfile(makeDefaultProfile());
    }
};
}  // namespace

TEST_CASE("tap: press and release produce one down and one up") {
    Rec r;
    CHECK(r.engine.onKey(0x20, true, 0));
    CHECK(r.engine.onKey(0x20, true, 10));  // auto-repeat ignored
    CHECK(r.engine.onKey(0x20, false, 20));
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.ev[1].action == MotionAction::Up);
    CHECK(r.ev[0].x == 1840);
    CHECK(r.ev[0].y == 800);
    CHECK(r.ev[0].frameWidth == 2000);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("unbound key is not consumed") {
    Rec r;
    CHECK_FALSE(r.engine.onKey('Q', true, 0));
    CHECK(r.ev.empty());
}

TEST_CASE("joystick diagonal is normalized and released when keys go up") {
    Rec r;
    r.engine.onKey('W', true, 0);
    r.engine.onKey('D', true, 0);
    REQUIRE(r.ev.size() == 3);  // down, move(up), move(up-right)
    const auto& m = r.ev.back();
    const float cx = 0.18f * 2000, cy = 0.72f * 1000, rad = 0.12f * 1000;
    CHECK(m.x == doctest::Approx(cx + rad / std::sqrt(2.f)).epsilon(0.01));
    CHECK(m.y == doctest::Approx(cy - rad / std::sqrt(2.f)).epsilon(0.01));
    r.engine.onKey('W', false, 0);
    r.engine.onKey('D', false, 0);
    CHECK(r.ev.back().action == MotionAction::Up);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("simultaneous bindings use distinct pointer ids") {
    Rec r;
    r.engine.onKey('W', true, 0);
    r.engine.onKey(0x20, true, 0);
    r.engine.onKey(0x01, true, 0);
    CHECK(r.engine.activeTouches() == 3);
    std::vector<uint64_t> downs;
    for (auto& e : r.ev)
        if (e.action == MotionAction::Down) downs.push_back(e.pointerId);
    REQUIRE(downs.size() == 3);
    CHECK(downs[0] != downs[1]);
    CHECK(downs[1] != downs[2]);
}

TEST_CASE("camera moves while locked and recenters at the aim ring edge") {
    Rec r;
    r.engine.onKey(0xC0, true, 0);
    r.engine.onKey(0xC0, false, 0);
    CHECK(r.engine.locked());
    r.engine.onMouseDelta(10, 0);
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.ev[0].pointerId == InputEngine::kCameraPointerId);
    CHECK(r.ev[1].x == 1410);
    r.ev.clear();
    // Ring-roam: the finger slides inside the aim ring (radius 0.25 * 1080 =
    // 270 px) and recenters seamlessly on the SAME pointer when it crosses.
    // 700 px of travel => resets at ~270 and ~540 px of accumulated drift.
    for (int i = 0; i < 70; ++i) r.engine.onMouseDelta(10, 0);
    int ups = 0, downs = 0;
    for (auto& e : r.ev) {
        ups += e.action == MotionAction::Up;
        downs += e.action == MotionAction::Down;
        CHECK(e.x >= 4);
        CHECK(e.x <= 1996);  // stays inside the frame
        // Same-pointer reset: the camera finger is always the primary id, never
        // a relay partner — two fingers on the camera zone broke the spray.
        CHECK(e.pointerId == InputEngine::kCameraPointerId);
    }
    CHECK(ups == 2);
    CHECK(downs == 2);
    r.engine.setLocked(false);
    CHECK(r.ev.back().action == MotionAction::Up);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("mouse delta ignored when unlocked") {
    Rec r;
    r.engine.onMouseDelta(10, 10);
    CHECK(r.ev.empty());
}

TEST_CASE("releaseAll lifts every finger (focus loss / disconnect)") {
    Rec r;
    r.engine.onKey('W', true, 0);
    r.engine.onKey(0x20, true, 0);
    r.engine.setLocked(true);
    r.engine.onMouseDelta(5, 5);
    CHECK(r.engine.activeTouches() == 3);
    r.ev.clear();
    r.engine.releaseAll();
    CHECK(r.engine.activeTouches() == 0);
    CHECK(r.ev.size() == 3);
    for (auto& e : r.ev) CHECK(e.action == MotionAction::Up);
    // keys were reset: releasing W later must not emit anything
    r.ev.clear();
    r.engine.onKey('W', false, 0);
    CHECK(r.ev.empty());
}

TEST_CASE("frame size change releases touches") {
    Rec r;
    r.engine.onKey(0x20, true, 0);
    r.engine.setFrameSize(1000, 2000);
    CHECK(r.engine.activeTouches() == 0);
    CHECK(r.ev.back().action == MotionAction::Up);
    // The release must carry the NEW dims: the server ignores events whose frame
    // size does not match the current video size (old dims = ghost finger).
    CHECK(r.ev.back().frameWidth == 1000);
    CHECK(r.ev.back().frameHeight == 2000);
}

TEST_CASE("drag interpolates from start to end") {
    Profile p;
    Binding d;
    d.name = "swipe";
    d.type = BindingType::Drag;
    d.key = 'E';
    d.pos = {0.1f, 0.5f};
    d.pos2 = {0.9f, 0.5f};
    d.durationMs = 100;
    p.bindings.push_back(d);
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(1000, 1000);
    e.setProfile(p);
    e.onKey('E', true, 0);
    e.tick(50000);
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].x == 500);
    e.tick(200000);
    CHECK(ev.back().x == 900);
    e.onKey('E', false, 0);
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(ev.back().x == 900);
}

TEST_CASE("no events without a frame size") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setProfile(makeDefaultProfile());
    e.onKey(0x20, true, 0);
    CHECK(ev.empty());
}

TEST_CASE("direct pointer: release is sent at the pressed position, not the frame origin") {
    Rec r;
    r.engine.directPointer(true, {0.5f, 0.25f});
    r.engine.directPointer(false, {0, 0});
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.ev[1].action == MotionAction::Up);
    CHECK(r.ev[1].pointerId == r.ev[0].pointerId);
    CHECK(r.ev[1].x == 1000);
    CHECK(r.ev[1].y == 250);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("direct pointer: release follows the last move and is sent once") {
    Rec r;
    r.engine.directPointer(true, {0.1f, 0.1f});
    r.engine.directMove({0.3f, 0.6f});
    r.engine.directPointer(false, {});
    r.engine.directPointer(false, {});
    REQUIRE(r.ev.size() == 3);
    CHECK(r.ev[2].action == MotionAction::Up);
    CHECK(r.ev[2].x == 600);
    CHECK(r.ev[2].y == 600);
}

TEST_CASE("direct pointer: releaseAll lifts the finger where it is") {
    Rec r;
    r.engine.directPointer(true, {0.75f, 0.5f});
    r.engine.releaseAll();
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[1].action == MotionAction::Up);
    CHECK(r.ev[1].x == 1500);
    CHECK(r.ev[1].y == 500);
}

TEST_CASE("Smart Fire: recoil pulls camera pointer smoothly while locked and toggles via hotkey") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.7f, 0.5f};
    cam.radius = 0.25f;
    p.bindings.push_back(cam);

    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 0x01;  // Mouse L
    fire.pos = {0.85f, 0.7f};
    fire.recoilEnabled = true;
    fire.recoilDown = 30;
    fire.recoilUp = 0;
    fire.recoilLeft = 0;
    fire.recoilRight = 10;
    fire.recoilDelayMs = 0;
    fire.recoilToggleKey = 0x73;  // F4
    p.bindings.push_back(fire);

    e.setProfile(p);
    e.setLocked(true);

    // Press Fire key -> fire finger DOWN first (shot lands ASAP), then the
    // camera finger lands on its anchor ready to own the recoil pull.
    CHECK(e.onKey(0x01, true, 100000));
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].action == MotionAction::Down);
    CHECK(ev[0].pointerId == InputEngine::kBindingPointerBase + 1);
    CHECK(ev[0].x == 1700);
    CHECK(ev[0].y == 700);
    CHECK(ev[1].action == MotionAction::Down);
    CHECK(ev[1].pointerId == InputEngine::kCameraPointerId);
    CHECK(ev[1].x == 1400);
    CHECK(ev[1].y == 500);

    // The primed accumulator emits the first compensation step on the very
    // next ~1 ms tick — the first shots no longer climb before recoil starts.
    e.tick(101000);
    REQUIRE(ev.size() >= 3);
    CHECK(ev.back().action == MotionAction::Move);
    CHECK(ev.back().pointerId == InputEngine::kCameraPointerId);
    CHECK(ev.back().y > 500);

    // Advance 50 ms -> recoil keeps pulling downward & right on the camera finger.
    e.tick(150000);
    REQUIRE(ev.size() >= 4);
    CHECK(ev.back().action == MotionAction::Move);
    CHECK(ev.back().pointerId == InputEngine::kCameraPointerId);
    CHECK(ev.back().y > 500);
    CHECK(ev.back().x > 1400);

    // Press Recoil Toggle Key (F4) -> disables recoil
    CHECK(e.onKey(0x73, true, 160000));
    CHECK(e.onKey(0x73, false, 165000));
    std::size_t idx = 99;
    bool recoilOn = true;
    CHECK(e.consumeRecoilToggle(idx, recoilOn));
    CHECK(idx == 1);
    CHECK_FALSE(recoilOn);

    // Release Fire key -> lifts fire finger cleanly
    CHECK(e.onKey(0x01, false, 200000));
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(ev.back().pointerId == InputEngine::kBindingPointerBase + 1);
}

TEST_CASE("Smart Fire: rapid fire pulses down/up while held") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 'F';
    fire.pos = {0.8f, 0.8f};
    fire.recoilEnabled = false;
    fire.rapidFire = true;
    fire.rapidFireDelayMs = 40;  // 20ms half-period
    p.bindings.push_back(fire);

    e.setProfile(p);
    e.onKey('F', true, 0);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].action == MotionAction::Down);

    e.tick(25000);  // > 20ms -> lifts finger
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].action == MotionAction::Up);

    e.tick(50000);  // another > 20ms -> presses finger again
    REQUIRE(ev.size() == 3);
    CHECK(ev[2].action == MotionAction::Down);

    e.onKey('F', false, 60000);
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(e.activeTouches() == 0);
}

TEST_CASE("Dual-key combo (H + T) triggers only when both keys are held together") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(1000, 1000);
    Profile p;
    Binding b;
    b.type = BindingType::Tap;
    b.key = 'H';
    b.key2 = 'T';
    b.pos = {0.5f, 0.5f};
    p.bindings.push_back(b);
    e.setProfile(p);

    e.onKey('H', true, 0);
    CHECK(ev.empty());  // H alone does not fire H+T
    e.onKey('T', true, 1000);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].action == MotionAction::Down);
    e.onKey('H', false, 2000);
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].action == MotionAction::Up);
    e.onKey('T', false, 3000);
}

TEST_CASE("Quick tap mode auto-releases while key remains held") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(1000, 1000);
    Profile p;
    Binding b;
    b.type = BindingType::Tap;
    b.key = 'G';
    b.quickTap = true;
    b.tapHoldMs = 40;
    b.pos = {0.4f, 0.6f};
    p.bindings.push_back(b);
    e.setProfile(p);

    e.onKey('G', true, 0);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].action == MotionAction::Down);
    e.tick(20000);  // 20 ms -> still down
    CHECK(ev.size() == 1);
    e.tick(45000);  // 45 ms -> auto-released
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].action == MotionAction::Up);
    e.onKey('G', false, 60000);  // no duplicate Up
    CHECK(ev.size() == 2);
}

TEST_CASE("multi-source ownership: macro cannot release a key held by physical input") {
    Rec r;
    r.engine.onKey(0x20, true, 0, InputSource::Physical);   // user holds Space
    REQUIRE(r.ev.size() == 1);
    r.engine.onKey(0x20, true, 10, InputSource::Macro);     // macro also presses Space
    CHECK(r.ev.size() == 1);                                // no duplicate Down
    r.engine.onKey(0x20, false, 20, InputSource::Macro);    // macro releases it
    CHECK(r.ev.size() == 1);                                // user still holds -> no Up
    CHECK(r.engine.keyState(0x20) == KeyLogicalState::Pressed);
    CHECK(r.engine.keySources(0x20) == static_cast<uint8_t>(InputSource::Physical));
    r.engine.onKey(0x20, false, 30, InputSource::Physical); // user releases
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[1].action == MotionAction::Up);
    CHECK(r.engine.sourceConflictsResolved() >= 2);
}

TEST_CASE("releaseSource frees only the keys owned by that source") {
    Rec r;
    r.engine.onKey(0x20, true, 0, InputSource::Physical);
    r.engine.onKey(0x20, true, 0, InputSource::Monitor);
    r.engine.onKey(0x01, true, 0, InputSource::Macro);
    r.engine.releaseSource(InputSource::Monitor, 0);
    CHECK(r.engine.keyState(0x20) == KeyLogicalState::Pressed);  // still held by Physical
    r.engine.releaseSource(InputSource::Physical, 0);
    CHECK(r.engine.keyState(0x20) == KeyLogicalState::Released);
    CHECK(r.engine.activeTouches() == 1);  // the macro-pressed mouse binding remains
    r.engine.releaseSource(InputSource::Macro, 0);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("macro fingers get distinct pointer ids and shared ownership") {
    Rec r;
    r.engine.onMacroTouch(MotionAction::Down, 0, {0.5f, 0.5f}, InputSource::Macro);
    r.engine.onMacroTouch(MotionAction::Down, 0, {0.5f, 0.5f}, InputSource::ImageClick);
    int downs = 0;
    for (auto& e : r.ev)
        if (e.action == MotionAction::Down) ++downs;
    CHECK(downs == 1);  // second source joined without a second Down
    r.engine.onMacroTouch(MotionAction::Up, 0, {0.5f, 0.5f}, InputSource::Macro);
    CHECK(r.ev.back().action != MotionAction::Up);  // still held by ImageClick
    r.engine.onMacroTouch(MotionAction::Up, 0, {0.5f, 0.5f}, InputSource::ImageClick);
    CHECK(r.ev.back().action == MotionAction::Up);
    CHECK(r.ev.back().pointerId == InputEngine::kMacroPointerBase + 0);
    CHECK(r.engine.activeTouches() == 0);
    r.engine.onMacroTouch(MotionAction::Down, 1, {0.3f, 0.3f}, InputSource::Macro);
    CHECK(r.ev.back().pointerId == InputEngine::kMacroPointerBase + 1);
    r.engine.releaseAll();
    CHECK(r.engine.activeTouches() == 0);
    CHECK(r.ev.back().action == MotionAction::Up);
}

TEST_CASE("sub-pixel camera deltas accumulate instead of being rounded away") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.7f, 0.5f};
    cam.radius = 0.25f;
    cam.sensitivity = 0.4f;  // each delta of 1 -> 0.4px, sub-pixel
    p.bindings.push_back(cam);
    e.setProfile(p);
    e.setLocked(true);
    e.onMouseDelta(1, 0);   // 0.4px: Down only, fractional part retained
    REQUIRE(ev.size() == 1);
    e.onMouseDelta(1, 0);   // 0.8px accumulated: still below 1px
    CHECK(ev.size() == 1);
    e.onMouseDelta(1, 0);   // 1.2px -> one Move of +1px
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].action == MotionAction::Move);
    CHECK(ev[1].x == ev[0].x + 1);
    e.setLocked(false);
}

static Profile camProfile(Profile p, Binding cam) {
    cam.type = BindingType::Camera;
    cam.pos = {0.5f, 0.5f};
    cam.radius = 0.25f;
    p.bindings.push_back(cam);
    return p;
}

TEST_CASE("Camera: independent X/Y sensitivity and immediate emit") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.sensitivity = 1.0f;
    cam.sensitivityY = 0.5f;
    cam.camAutoReleaseMs = 0;  // keep the finger held for this emit-focused test
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    e.onMouseDelta(10, 0, 1000);   // leading edge: Down + immediate Move
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].x == ev[0].x + 10);
    // Direct 1:1 emit: every delta goes out immediately — no send-rate hold, so
    // the second delta needs no tick() to flush it (direct, "solid" feel).
    e.onMouseDelta(0, 10, 2000);
    REQUIRE(ev.size() == 3);
    CHECK(ev[2].action == MotionAction::Move);
    CHECK(ev[2].y == ev[0].y + 5);  // Y uses its own sensitivity
    CHECK(ev[2].x == ev[0].x + 10); // final position keeps the earlier X delta
    e.setLocked(false);
}

TEST_CASE("Camera: bounds clamp keeps pointer inside the rectangle") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.camBoundsEnabled = true;
    cam.camBoundsRange = 0.10f;  // 100 px radius on a 1000px-tall frame
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    for (int i = 0; i < 50; ++i) e.onMouseDelta(20, 0, 1000 + i);
    // Pointer must be clamped to 100px from the 1000,500 anchor.
    const float ax = 1000.0f, ay = 500.0f;
    const float d = std::hypot(ev.back().x - ax, ev.back().y - ay);
    CHECK(d <= 100.5f);
    // No recenter Up/Down cycle should have occurred (bounds never lift the finger).
    for (const auto& t : ev)
        if (t.pointerId == InputEngine::kCameraPointerId) CHECK(t.action != MotionAction::Up);
    e.setLocked(false);
}

TEST_CASE("Camera: averaging preserves total motion and flushes stale deltas") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.camAverageMs = 20;
    cam.camAutoReleaseMs = 0;  // this test spans >40 ms idle ticks on purpose
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    e.onMouseDelta(10, 0, 1000000);
    e.onMouseDelta(10, 0, 1010000);
    // Only the anchor Down is emitted; the deltas are held by the smoothing window.
    CHECK(ev.size() == 1);
    e.tick(1016000);
    e.tick(1032000);
    e.tick(1100000);  // window elapsed: remainder flushes fully
    REQUIRE(ev.back().action == MotionAction::Move);
    CHECK(ev.back().x == 1020);  // all 20px of motion delivered, just spread over the window
    e.setLocked(false);
}

TEST_CASE("Camera: return to center instant after idle delay") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.camBoundsEnabled = true;  // keep finger inside, no recenter noise
    cam.camBoundsRange = 0.40f;
    cam.camReturnType = 1;
    cam.camReturnDelayMs = 100;
    cam.camAutoReleaseMs = 0;  // return-to-center needs the finger to stay held
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    e.onMouseDelta(50, 0, 1000000);
    e.tick(1050000);   // still inside delay
    CHECK(ev.back().x > 1050.0f - 1.0f);
    e.tick(1200000);   // 200ms idle > 100ms delay -> teleport back
    // The return MUST be Up-then-Down at the anchor, never a backward Move
    // (a Move while the finger is down drags the camera in the opposite direction).
    REQUIRE(ev.size() >= 2);
    CHECK(ev[ev.size() - 2].action == MotionAction::Up);
    CHECK(ev.back().action == MotionAction::Down);
    CHECK(ev.back().x == 1000.0f);
    CHECK(ev.back().y == 500.0f);
    for (const auto& t : ev)
        CHECK(!(t.action == MotionAction::Move && t.x < 1050.0f));  // no camera-dragging return
    e.setLocked(false);
}

TEST_CASE("Camera: auto fire pulses on the fire anchor and releases on unlock") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.5f, 0.5f};
    cam.radius = 0.25f;
    cam.camAutoFire = true;
    cam.camAutoFireReleaseMs = 50;
    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 0x01;
    fire.pos = {0.2f, 0.8f};
    Profile p;
    p.bindings.push_back(cam);
    p.bindings.push_back(fire);
    e.setProfile(p);
    e.setLocked(true);
    e.onMouseDelta(1, 0, 1000000);  // aim becomes active
    e.tick(1001000);                // first pulse
    REQUIRE(ev.size() >= 3);
    CHECK(ev.back().pointerId == InputEngine::kAutoFirePointerId);
    CHECK(ev.back().action == MotionAction::Down);
    CHECK(ev.back().x == 400);   // 0.2 * 2000
    CHECK(ev.back().y == 800);   // 0.8 * 1000
    e.tick(1060000);             // > 50ms later: Up
    CHECK(ev.back().action == MotionAction::Up);
    e.setLocked(false);          // unlocking must lift any held pulse
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(ev.back().pointerId == InputEngine::kCameraPointerId);
    // No auto-fire finger left down anywhere.
    for (const auto& t : ev)
        CHECK(!(t.pointerId == InputEngine::kAutoFirePointerId && t.action == MotionAction::Down &&
                &t == &ev.back()));
}

TEST_CASE("all synthetic touches report zero pressure (WASD+ parity)") {
    Rec r;
    r.engine.onKey(0xC0, true, 0);
    r.engine.onKey(0xC0, false, 0);  // locked
    r.engine.onMouseDelta(10, 0, 1000);
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.ev[0].pressure == 0.0f);   // WASD+ parity: Prs 0.0 on the device
    CHECK(r.ev[1].action == MotionAction::Move);
    CHECK(r.ev[1].pressure == 0.0f);
    r.engine.onKey(0x20, true, 2000);  // every synthetic finger reports 0, not 1.0
    CHECK(r.ev.back().pressure == 0.0f);
    r.engine.onKey('W', true, 3000);   // joystick finger too
    CHECK(r.ev.back().pressure == 0.0f);
    r.engine.setLocked(false);
}

TEST_CASE("Camera: auto-release lifts the idle finger and the next delta re-lands it centered") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.camAutoReleaseMs = 40;
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    e.onMouseDelta(10, 0, 1000000);
    REQUIRE(ev.size() == 2);  // Down + Move
    e.tick(1030000);          // 30 ms idle < 40 ms: still held
    CHECK(ev.size() == 2);
    e.tick(1050000);          // 50 ms idle -> finger lifted
    REQUIRE(ev.size() == 3);
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(ev.back().pointerId == InputEngine::kCameraPointerId);
    // Next motion re-lands the finger on the anchor, then moves immediately.
    e.onMouseDelta(20, 0, 2000000);
    REQUIRE(ev.size() == 5);
    CHECK(ev[3].action == MotionAction::Down);
    CHECK(ev[3].x == 1000);
    CHECK(ev[3].y == 500);
    CHECK(ev[4].action == MotionAction::Move);
    e.setLocked(false);
    CHECK(e.activeTouches() == 0);
}

TEST_CASE("Camera: auto-release is suppressed while a fire key is held") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.5f, 0.5f};
    cam.radius = 0.25f;
    cam.camAutoReleaseMs = 40;
    p.bindings.push_back(cam);
    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 'F';
    fire.pos = {0.8f, 0.8f};
    fire.recoilEnabled = false;  // pure hold: the camera finger stays put
    p.bindings.push_back(fire);
    e.setProfile(p);
    e.setLocked(true);
    e.onMouseDelta(10, 0, 1000000);
    e.onKey('F', true, 1005000);
    const std::size_t atPress = ev.size();
    e.tick(1300000);  // ~300 ms idle but fire is held: no camera auto-release
    for (std::size_t i = atPress; i < ev.size(); ++i)
        CHECK(!(ev[i].pointerId == InputEngine::kCameraPointerId && ev[i].action == MotionAction::Up));
    // After the fire key releases, the idle finger auto-releases normally.
    e.onKey('F', false, 1400000);
    e.tick(1500000);
    CHECK(ev.back().action == MotionAction::Up);
    CHECK(ev.back().pointerId == InputEngine::kCameraPointerId);
    e.setLocked(false);
    CHECK(e.activeTouches() == 0);
}

TEST_CASE("Camera: rectangular bounds clamp X and Y independently") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Binding cam;
    cam.camBoundsEnabled = true;
    cam.camBoundsRange = 0.10f;   // Y half-extent: 100 px on a 1000 px-tall frame
    cam.camBoundsRangeX = 0.40f;  // X half-extent: 400 px — wide like WASD+
    e.setProfile(camProfile(Profile{}, cam));
    e.setLocked(true);
    for (int i = 0; i < 40; ++i) e.onMouseDelta(20, 0, 1000 + i);  // +800 px intent
    CHECK(ev.back().x == 1400);   // clamped at anchor+400, not the old 100 circle
    CHECK(ev.back().y == 500);
    for (int i = 0; i < 20; ++i) e.onMouseDelta(0, 20, 2000 + i);  // +400 px intent
    CHECK(ev.back().y == 600);    // clamped at anchor+100
    e.setLocked(false);
}

TEST_CASE("Smart Fire: press leaves the wandering camera finger untouched") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.5f, 0.5f};
    cam.radius = 0.25f;
    cam.camAutoReleaseMs = 0;
    p.bindings.push_back(cam);
    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 0x01;
    fire.pos = {0.85f, 0.7f};
    fire.recoilEnabled = true;
    fire.recoilDown = 30;
    p.bindings.push_back(fire);
    e.setProfile(p);
    e.setLocked(true);
    e.onMouseDelta(0, 50, 1000000);  // camera finger wanders 50 px off its anchor
    REQUIRE(ev.size() == 2);
    e.onKey(0x01, true, 1010000);
    // Fire Down is emitted, and the wandering camera finger is LEFT ALONE — the
    // fire and aim fingers stay fully independent. Recentering only happens at
    // the aim ring edge, never on a trigger press.
    REQUIRE(ev.size() == 3);
    CHECK(ev[2].action == MotionAction::Down);   // fire finger only, no camera teleport
    CHECK(ev[2].pointerId != InputEngine::kCameraPointerId);
    e.onKey(0x01, false, 1100000);
    e.setLocked(false);
    CHECK(e.activeTouches() == 0);
}

TEST_CASE("tick is monotonic: a stale timestamp cannot rewind recoil") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding cam;
    cam.type = BindingType::Camera;
    cam.pos = {0.5f, 0.5f};
    cam.radius = 0.25f;
    cam.camAutoReleaseMs = 0;
    p.bindings.push_back(cam);
    Binding fire;
    fire.type = BindingType::Fire;
    fire.key = 0x01;
    fire.pos = {0.85f, 0.7f};
    fire.recoilEnabled = true;
    fire.recoilDown = 30;
    p.bindings.push_back(fire);
    e.setProfile(p);
    e.setLocked(true);
    e.onKey(0x01, true, 100000);
    // A stale timestamp (as if a slower caller read its clock before the mutex)
    // must clamp, not rewind lastTickUs or emit a backwards/giant recoil step.
    e.tick(200000);
    e.tick(150000);  // stale: clamped to 200000 -> dtUs 0
    e.tick(300000);
    int lastY = -1;
    for (const auto& t : ev) {
        if (t.pointerId == InputEngine::kCameraPointerId && t.action == MotionAction::Move) {
            CHECK(t.y >= lastY);  // recoilDown only ever increases y
            lastY = t.y;
        }
    }
    CHECK(lastY > 500);  // recoil did pull down overall
    e.onKey(0x01, false, 400000);
    e.setLocked(false);
    CHECK(e.activeTouches() == 0);
}

TEST_CASE("Camera: repeated edge resets stay on one pointer with no leaks") {
    Rec r;
    r.engine.setLocked(true);
    for (int i = 0; i < 200; ++i) r.engine.onMouseDelta(10, 0, 1000 + i);  // 2000 px: many resets
    r.engine.setLocked(false);
    int camDowns = 0, camUps = 0;
    for (const auto& e : r.ev) {
        CHECK(e.pointerId == InputEngine::kCameraPointerId);  // never a relay partner id
        if (e.action == MotionAction::Down) ++camDowns;
        if (e.action == MotionAction::Up) ++camUps;
    }
    CHECK(camDowns == camUps);  // balanced: nothing left held on the device
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("Multi-touch: toggle finger lands on first press, lifts on second") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (Binding& b : p.bindings)
        if (b.type == BindingType::Tap && b.key == 0x20) b.tapToggle = true;
    r.engine.setProfile(p);
    r.engine.onKey(0x20, true, 0);   // first press -> finger down
    r.engine.onKey(0x20, false, 10); // key-up must NOT release a toggle finger
    REQUIRE(r.ev.size() == 1);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.engine.fingerStates().size() == 1);
    r.engine.onKey(0x20, true, 20);  // second press -> finger up
    REQUIRE(r.ev.size() == 2);
    CHECK(r.ev[1].action == MotionAction::Up);
    CHECK(r.engine.activeFingerCount() == 0);
}

TEST_CASE("Multi-touch: tapRepeat emits N pulses then stops cleanly") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding b;
    b.type = BindingType::Tap;
    b.key = 'T';
    b.pos = {0.5f, 0.5f};
    b.tapRepeat = 3;
    p.bindings.push_back(b);
    e.setProfile(p);
    e.onKey('T', true, 1000000);
    e.onKey('T', false, 1040000);
    // tapRepeat owns its release: the programmed 3-tap train completes through
    // tick() even though the key was released mid-sequence (Down,Up x3).
    const int64_t t0 = 1100000;
    for (int k = 0; k < 20; ++k) e.tick(t0 + k * 50000);
    int downs = 0, ups = 0;
    for (const auto& t : ev) t.action == MotionAction::Down ? ++downs : t.action == MotionAction::Up ? ++ups : 0;
    CHECK(downs == 3);
    CHECK(ups == 3);
    CHECK(e.activeFingerCount() == 0);
}

TEST_CASE("Multi-touch: finger budget rejects excess presses without partial events") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    // Budget 3: one slot is reserved for the direct pointer, leaving room for
    // exactly two non-core tap fingers.
    p.maxVirtualFingers = 3;
    for (int i = 0; i < 4; ++i) {
        Binding b;
        b.type = BindingType::Tap;
        b.key = 'A' + i;
        b.pos = {0.2f + 0.2f * i, 0.5f};
        p.bindings.push_back(b);
    }
    e.setProfile(p);
    for (int k = 0; k < 4; ++k) e.onKey('A' + k, true, 1000000 + k);
    // Only the first two fingers went down; the rest were refused.
    int downs = 0;
    for (const auto& t : ev) if (t.action == MotionAction::Down) ++downs;
    CHECK(downs == 2);
    CHECK(e.activeFingerCount() == 2);
    CHECK(e.sourceConflictsResolved() >= 2);
    // Releasing refused keys must emit no spurious Up.
    const std::size_t before = ev.size();
    e.onKey('C', false, 2000000);
    e.onKey('D', false, 2000001);
    CHECK(ev.size() == before);
    e.onKey('A', false, 2000002);
    CHECK(ev.back().action == MotionAction::Up);
    e.releaseAll();
    CHECK(e.activeFingerCount() == 0);
}

TEST_CASE("Multi-touch: disabled binding is ignored, enabled fires normally") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    Binding b;
    b.type = BindingType::Tap;
    b.key = 'G';
    b.pos = {0.5f, 0.5f};
    b.disabled = true;
    p.bindings.push_back(b);
    e.setProfile(p);
    CHECK_FALSE(e.onKey('G', true, 1000000));
    CHECK_FALSE(e.onKey('G', false, 1000000));  // key stays logically held otherwise
    CHECK(ev.empty());
    Profile p2 = p;
    p2.bindings[0].disabled = false;
    e.setProfile(p2);
    CHECK(e.onKey('G', true, 1000000));
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].action == MotionAction::Down);
}

TEST_CASE("Multi-touch: fingerStates reports concurrent independent fingers") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    for (int i = 0; i < 3; ++i) {
        Binding b;
        b.type = BindingType::Tap;
        b.key = 'M' + i;
        b.pos = {0.2f * (i + 1), 0.4f};
        p.bindings.push_back(b);
    }
    e.setProfile(p);
    e.onKey('M', true, 1000);
    e.onKey('N', true, 1000);
    e.onKey('O', true, 1000);
    const auto st = e.fingerStates();
    REQUIRE(st.size() == 3);
    // Each finger has a distinct stable pointer id.
    CHECK(st[0].pointerId != st[1].pointerId);
    CHECK(st[1].pointerId != st[2].pointerId);
    CHECK(st[0].pointerId != st[2].pointerId);
    // Releasing the middle finger must not disturb the others.
    e.onKey('N', false, 2000);
    const auto st2 = e.fingerStates();
    CHECK(st2.size() == 2);
    int downs = 0, ups = 0;
    for (const auto& t : ev) t.action == MotionAction::Down ? ++downs : t.action == MotionAction::Up ? ++ups : 0;
    CHECK(downs == 3);
    CHECK(ups == 1);  // exactly one release - the middle finger
    e.releaseAll();
    CHECK(e.fingerStates().empty());
}

TEST_CASE("Advanced Dpad: Up offset, Shift walk offset, and Release delay") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(1000, 1000);
    Profile p;
    Binding joy;
    joy.type = BindingType::Joystick;
    joy.pos = {0.5f, 0.5f};
    joy.radius = 0.20f;
    joy.joyUpOffset = 0.10f;        // +10% upward reach
    joy.joyShiftEnabled = true;
    joy.joyShiftKey = 0x10;         // Shift
    joy.joyShiftOffset = -0.10f;    // -10% walk radius
    joy.joyReleaseDelayMs = 50;     // 50ms release delay
    p.bindings.push_back(joy);
    e.setProfile(p);

    // Press W -> moves up by radius(200) + upOffset(100) = 300 px above center (y = 200)
    e.onKey('W', true, 0);
    REQUIRE(ev.size() >= 2);
    CHECK(ev.back().y == 200);

    // Hold Shift while W is pressed -> radius becomes 0.10 (100px) + upOffset(100px) -> y = 300
    e.onKey(0x10, true, 1000);
    CHECK(ev.back().y == 300);
    e.onKey(0x10, false, 2000);

    // Release W -> finger stays down until 50ms release delay elapses
    const std::size_t before = ev.size();
    e.onKey('W', false, 10000);
    CHECK(ev.size() == before);  // not lifted yet
    e.tick(30000);
    CHECK(ev.size() == before);
    e.tick(65000);
    REQUIRE(ev.size() == before + 1);
    CHECK(ev.back().action == MotionAction::Up);
}
TEST_CASE("joystick W+D at 60deg emits correct diagonal, W resumes on D release") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) b.joyAngleDeg = 60.0f;
    r.engine.setProfile(p);
    const float cx = 0.18f * 2000, cy = 0.72f * 1000, rad = 0.12f * 1000;

    r.engine.onKey('W', true, 0);
    REQUIRE(!r.ev.empty());
    CHECK(r.ev.back().x == doctest::Approx(cx).epsilon(0.01));
    CHECK(r.ev.back().y == doctest::Approx(cy - rad).epsilon(0.01));

    r.engine.onKey('D', true, 1000);
    // 60 deg deflection from vertical: dir = (sin60, cos60) = (0.866, 0.5)
    CHECK(r.ev.back().x == doctest::Approx(cx + 0.866f * rad).epsilon(0.02));
    CHECK(r.ev.back().y == doctest::Approx(cy - 0.5f * rad).epsilon(0.02));
    CHECK(r.engine.keyState('W') == KeyLogicalState::Pressed);
    CHECK(r.engine.keyState('D') == KeyLogicalState::Pressed);

    r.engine.onKey('D', false, 2000);
    CHECK(r.ev.back().x == doctest::Approx(cx).epsilon(0.01));
    CHECK(r.ev.back().y == doctest::Approx(cy - rad).epsilon(0.01));

    r.engine.onKey('W', false, 3000);
    CHECK(r.ev.back().action == MotionAction::Up);
}

TEST_CASE("setProfile preserves live fingers when only runtime fields change") {
    Rec r;
    r.engine.onKey('W', true, 0);
    REQUIRE(r.engine.activeTouches() == 1);
    // Simulate a hotkey toggle mutating ONLY a runtime field (recoilEnabled/rapidFire).
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Fire) { b.recoilEnabled = !b.recoilEnabled; break; }
    r.engine.setProfile(p);
    CHECK(r.engine.activeTouches() == 1);  // joystick finger must survive the toggle
    r.engine.onKey('W', false, 0);
    CHECK(r.ev.back().action == MotionAction::Up);
}

TEST_CASE("setProfile releases only the structurally changed binding") {
    Rec r;
    r.engine.onKey('W', true, 0);
    r.engine.onKey(0x20, true, 0);  // Space -> tap finger
    REQUIRE(r.engine.activeTouches() == 2);
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.key == 0x20) { b.pos.x = 0.5f; break; }  // move Space's tap binding
    r.engine.setProfile(p);
    CHECK(r.engine.activeTouches() == 1);  // only the tap finger lifted; joystick kept
    r.engine.onKey('W', false, 0);
    CHECK(r.ev.back().action == MotionAction::Up);
}

TEST_CASE("quick tap: early key release does not cut the dwell short") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(1000, 1000);
    Profile p;
    Binding b;
    b.type = BindingType::Tap;
    b.key = 'G';
    b.quickTap = true;
    b.tapHoldMs = 40;
    b.pos = {0.4f, 0.6f};
    p.bindings.push_back(b);
    e.setProfile(p);

    e.onKey('G', true, 0);
    REQUIRE(ev.size() == 1);
    e.onKey('G', false, 20000);  // released at 20ms < 40ms dwell
    CHECK(ev.size() == 1);       // finger must stay down until the timer
    e.tick(50000);
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].action == MotionAction::Up);
    CHECK(e.activeFingerCount() == 0);
}

TEST_CASE("finger budget reserves slots so movement always lands") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    p.maxVirtualFingers = 4;
    Binding joy;
    joy.type = BindingType::Joystick;
    joy.pos = {0.2f, 0.7f};
    joy.radius = 0.12f;
    p.bindings.push_back(joy);
    for (int i = 0; i < 2; ++i) {  // two taps can fill the whole non-core allowance
        Binding t;
        t.type = BindingType::Tap;
        t.key = 'F' + i;
        t.pos = {0.9f, 0.5f + 0.1f * i};
        p.bindings.push_back(t);
    }
    e.setProfile(p);
    // Reserved: joystick(1) + direct(1) -> non-core room = 4 - 2 = 2.
    e.onKey('F', true, 0);
    e.onKey('G', true, 0);
    CHECK(e.fingerRejections() == 0);
    // A third non-core finger is refused, but the joystick must still activate.
    e.onMacroTouch(MotionAction::Down, 0, {0.5f, 0.5f});
    CHECK(e.fingerRejections() == 1);
    e.onKey('W', true, 0);
    REQUIRE(!ev.empty());
    CHECK(ev.back().action == MotionAction::Move);
    CHECK(ev.back().pointerId == InputEngine::kBindingPointerBase + 0);
}

TEST_CASE("joystick retries activation when the finger budget frees up") {
    std::vector<proto::TouchEvent> ev;
    InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
    e.setFrameSize(2000, 1000);
    Profile p;
    p.maxVirtualFingers = 1;
    Binding joy;
    joy.type = BindingType::Joystick;
    joy.pos = {0.2f, 0.7f};
    joy.radius = 0.12f;
    p.bindings.push_back(joy);
    e.setProfile(p);
    e.directPointer(true, {0.5f, 0.5f});  // consumes the single slot
    REQUIRE(ev.size() == 1);
    e.onKey('W', true, 0);  // budget full: rejected without a partial Down
    CHECK(ev.size() == 1);
    CHECK(e.fingerRejections() == 1);
    e.directPointer(false, {});
    ev.clear();
    e.tick(50000);   // before the 100ms retry point: nothing yet
    CHECK(ev.empty());
    e.tick(150000);  // retry fires: the held direction key lands the finger
    REQUIRE(ev.size() >= 2);
    CHECK(ev[0].action == MotionAction::Down);
    CHECK(ev[0].x == 400);  // 0.2 * 2000
    CHECK(ev[0].y == 700);  // 0.7 * 1000
    CHECK(ev[1].action == MotionAction::Move);
}

TEST_CASE("non-humanized joystick re-emits its position as a heartbeat") {
    Rec r;  // default profile joystick is non-humanized
    r.engine.onKey('W', true, 0);
    REQUIRE(r.ev.size() == 2);  // down + move
    r.engine.tick(10000);
    CHECK(r.ev.size() == 2);    // inside the 30ms heartbeat interval
    r.engine.tick(50000);
    REQUIRE(r.ev.size() == 3);
    CHECK(r.ev.back().action == MotionAction::Move);
    CHECK(r.ev.back().pointerId == InputEngine::kBindingPointerBase + 0);
    r.engine.onKey('W', false, 60000);
    CHECK(r.ev.back().action == MotionAction::Up);
}

// ---- Natural movement profile (joyProfile == 2) ----------------------------------

TEST_CASE("natural joystick: Down, stepped Moves, exact target, single Up") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) b.joyProfile = 2;
    r.engine.setProfile(p);
    const float cx = 0.18f * 2000, cy = 0.72f * 1000, rad = 0.12f * 1000;

    r.engine.onKey('W', true, 0);
    REQUIRE(r.ev.size() == 1);
    CHECK(r.ev[0].action == MotionAction::Down);
    CHECK(r.ev[0].x == doctest::Approx(cx));
    CHECK(r.ev[0].y == doctest::Approx(cy));

    // Initial transition: no Move before bornUs + joyNatInitialMs (12 ms).
    r.engine.tick(5000);
    r.engine.tick(11000);
    CHECK(r.ev.size() == 1);

    // Drive ticks well past the 90 ms travel duration.
    for (int64_t t = 12000; t <= 300000; t += 1000) r.engine.tick(t);

    int downs = 0, ups = 0;
    float prevProg = -1.0f;  // along-axis progress: cy - y must be non-decreasing (monotonic)
    for (const auto& e : r.ev) {
        if (e.action == MotionAction::Down) ++downs;
        if (e.action == MotionAction::Up) ++ups;
        if (e.action == MotionAction::Move) {
            const float prog = cy - e.y;  // W = upward stick: progress grows 0 -> rad
            CHECK_MESSAGE(prog >= prevProg - 0.01f, "reversal along direction");
            prevProg = prog;
        }
    }
    CHECK(downs == 1);   // no unintended re-down during the path
    CHECK(ups == 0);     // still held
    CHECK(r.ev.back().x == doctest::Approx(cx));
    CHECK(r.ev.back().y == doctest::Approx(cy - rad));  // converged exactly

    r.engine.onKey('W', false, 300000);
    CHECK(r.ev.back().action == MotionAction::Up);
    CHECK(r.engine.activeTouches() == 0);
}

TEST_CASE("natural joystick produces several small steps, not a single snap") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) {
            b.joyProfile = 2;
            b.joyNatSegMs = 10;
            b.joyNatDurMs = 80;
            b.joyNatTimeVarPct = 0;
            b.joyNatPathVarPx = 0.0f;
        }
    r.engine.setProfile(p);

    r.engine.onKey('W', true, 0);
    for (int64_t t = 12000; t <= 120000; t += 500) r.engine.tick(t);

    int movesToTarget = 0;
    bool reached = false;
    for (const auto& e : r.ev) {
        if (e.action != MotionAction::Move || reached) continue;
        ++movesToTarget;
        const float cy = 0.72f * 1000, rad = 0.12f * 1000;
        if (std::fabs(e.y - (cy - rad)) < 0.6f) reached = true;
    }
    CHECK(reached);
    // 80 ms at 10 ms cadence -> roughly 6-9 intermediate moves before the target.
    CHECK(movesToTarget >= 4);
    CHECK(movesToTarget <= 14);
}

TEST_CASE("natural joystick is deterministic for identical input timelines") {
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) {
            b.joyProfile = 2;
            b.joyNatTimeVarPct = 40;   // jitter on: the fixed seed must still repeat
            b.joyNatPathVarPx = 5.0f;
        }
    auto run = [&]() {
        Rec r;
        r.engine.setProfile(p);
        r.engine.onKey('W', true, 0);
        for (int64_t t = 5000; t <= 200000; t += 700) r.engine.tick(t);
        r.engine.onKey('W', false, 200000);
        return r.ev;
    };
    const auto a = run();
    const auto b = run();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].action == b[i].action);
        CHECK(a[i].x == doctest::Approx(b[i].x));
        CHECK(a[i].y == doctest::Approx(b[i].y));
    }
}

TEST_CASE("natural joystick lateral offset stays inside the configured bound") {
    Rec r;
    Profile p = makeDefaultProfile();
    const float var = 4.0f;
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) {
            b.joyProfile = 2;
            b.joyNatPathVarPx = var;
        }
    r.engine.setProfile(p);
    const float cx = 0.18f * 2000;

    r.engine.onKey('W', true, 0);
    for (int64_t t = 1000; t <= 200000; t += 500) r.engine.tick(t);
    // Pure vertical path: lateral offset shows up on x only.
    for (const auto& e : r.ev)
        if (e.action == MotionAction::Move) CHECK(std::fabs(e.x - cx) <= var + 0.6f);
}

TEST_CASE("natural joystick segment spacing caps per-step advance") {
    Rec r;
    Profile p = makeDefaultProfile();
    const float cx = 0.18f * 2000, cy = 0.72f * 1000;
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) {
            b.joyProfile = 2;
            b.joyNatSegMs = 10;
            b.joyNatDurMs = 60;
            b.joyNatSpacingPx = 15.0f;  // max ~15 px along-path per emit
            b.joyNatTimeVarPct = 0;
            b.joyNatPathVarPx = 0.0f;
        }
    r.engine.setProfile(p);

    r.engine.onKey('W', true, 0);
    float prevY = cy;
    for (int64_t t = 12000; t <= 200000; t += 500) {
        r.engine.tick(t);
        if (!r.ev.empty() && r.ev.back().action == MotionAction::Move) {
            const float step = std::fabs(prevY - r.ev.back().y);
            CHECK_MESSAGE(step <= 15.6f, "step advance exceeded spacing cap");
            prevY = static_cast<float>(r.ev.back().y);
        }
    }
    // Spacing cap extends the travel: 120 px at 15 px/step needs ~8 steps.
    CHECK(std::fabs(r.ev.back().y - (cy - 0.12f * 1000)) < 0.6f);
    CHECK(cx == doctest::Approx(r.ev.back().x));
}

TEST_CASE("natural joystick retargets smoothly on direction change") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) {
            b.joyProfile = 2;
            b.joyNatPathVarPx = 0.0f;
            b.joyNatTimeVarPct = 0;
        }
    r.engine.setProfile(p);
    const float cx = 0.18f * 2000, cy = 0.72f * 1000, rad = 0.12f * 1000;

    r.engine.onKey('W', true, 0);
    for (int64_t t = 1000; t <= 150000; t += 500) r.engine.tick(t);  // converged
    const std::size_t before = r.ev.size();
    r.engine.onKey('D', true, 150000);                              // W+D diagonal
    for (int64_t t = 151000; t <= 300000; t += 500) r.engine.tick(t);

    int downs = 0;
    for (std::size_t i = before; i < r.ev.size(); ++i)
        if (r.ev[i].action == MotionAction::Down) ++downs;
    CHECK(downs == 0);  // retarget must never re-down the finger

    // Default 45 deg deflection: dir = (sin45, cos45).
    CHECK(r.ev.back().x == doctest::Approx(cx + 0.7071f * rad).epsilon(0.03));
    CHECK(r.ev.back().y == doctest::Approx(cy - 0.7071f * rad).epsilon(0.03));
}

TEST_CASE("natural joystick releases cleanly mid-path") {
    Rec r;
    Profile p = makeDefaultProfile();
    for (auto& b : p.bindings)
        if (b.type == BindingType::Joystick) b.joyProfile = 2;
    r.engine.setProfile(p);

    r.engine.onKey('W', true, 0);
    r.engine.tick(20000);   // mid-path, not yet at target
    r.engine.tick(40000);
    r.engine.onKey('W', false, 45000);
    CHECK(r.ev.back().action == MotionAction::Up);
    CHECK(r.engine.activeTouches() == 0);
    int ups = 0, downs = 0;
    for (const auto& e : r.ev) {
        ups += e.action == MotionAction::Up;
        downs += e.action == MotionAction::Down;
    }
    CHECK(ups == 1);
    CHECK(downs == 1);
}

TEST_CASE("camera sensitivity is device-space invariant across stream resolutions") {
    // The same mouse flick at the same sensitivity must travel the same DEVICE
    // distance whether the stream is 800px or 1600px wide (WASD+ semantics).
    auto flick = [](uint32_t fw, uint32_t fh) {
        std::vector<proto::TouchEvent> ev;
        InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
        e.setFrameSize(fw, fh);
        e.setDeviceSize(1544, 720);  // physical landscape display
        Profile p;
        Binding cam;
        cam.type = BindingType::Camera;
        cam.pos = {0.5f, 0.5f};
        cam.radius = 1.0f;  // ring far away: no recenter during the flick
        cam.sensitivity = 1.0f;
        cam.sensitivityY = 1.0f;
        p.bindings.push_back(cam);
        e.setProfile(p);
        e.setLocked(true);
        e.onMouseDelta(200, 0, 1000);
        return ev;
    };
    const auto a = flick(800, 360);
    const auto b = flick(1600, 720);
    REQUIRE(a.size() == 2);  // Down + one Move
    REQUIRE(b.size() == 2);
    const double devA = (a[1].x - 0.5 * 800) * 1544.0 / 800.0;
    const double devB = (b[1].x - 0.5 * 1600) * 1544.0 / 1600.0;
    CHECK(devA == doctest::Approx(200.0).epsilon(0.05));
    CHECK(devB == doctest::Approx(200.0).epsilon(0.05));
    CHECK(devA == doctest::Approx(devB).epsilon(0.02));
}

TEST_CASE("recoil pull is not shrunk by device-pixel normalization") {
    // Regression: feeding frame-sized recoil deltas into the device-pixel path
    // multiplied them by frameW/devW a second time (~0.5x on 480p). The camera
    // pull must be identical with and without device size set.
    auto pull = [](bool withDev) {
        std::vector<proto::TouchEvent> ev;
        InputEngine e([&](const proto::TouchEvent& t) { ev.push_back(t); });
        e.setFrameSize(800, 400);
        if (withDev) e.setDeviceSize(1600, 800);
        Profile p;
        Binding cam;
        cam.type = BindingType::Camera;
        cam.pos = {0.5f, 0.3f};
        cam.radius = 1.0f;
        p.bindings.push_back(cam);
        Binding fire;
        fire.type = BindingType::Fire;
        fire.key = 0x01;
        fire.pos = {0.9f, 0.8f};
        fire.recoilEnabled = true;
        fire.recoilDown = 30;
        fire.recoilDelayMs = 0;
        p.bindings.push_back(fire);
        e.setProfile(p);
        e.setLocked(true);
        e.onKey(0x01, true, 0);
        ev.clear();
        for (int i = 0; i < 200; ++i) e.tick((i + 1) * 1000);  // 200 ms of ticks
        int lastY = -1;
        for (const auto& t : ev)
            if (t.pointerId == InputEngine::kCameraPointerId && t.action == MotionAction::Move) lastY = t.y;
        return lastY;
    };
    const int anchorY = static_cast<int>(0.3 * 400);  // 120
    const int withDev = pull(true);
    const int noDev = pull(false);
    // Expected pull over 200 ms: 30 * 0.00275 * 400 * 0.2 ~= 6.6 frame px (+1 px prime).
    CHECK(withDev > anchorY + 4);
    CHECK(withDev < anchorY + 12);
    CHECK(withDev == doctest::Approx(noDev).epsilon(0.15));
}
