#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/automation.h"
#include "core/profile.h"

using namespace wasd;

namespace {
struct Img {
    int w, h;
    std::vector<uint8_t> px;
    Img(int w_, int h_, uint8_t r, uint8_t g, uint8_t b) : w(w_), h(h_), px(size_t(w_) * h_ * 3) {
        for (int i = 0; i < w * h; ++i) px[i * 3] = r, px[i * 3 + 1] = g, px[i * 3 + 2] = b;
    }
    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &px[(size_t(y) * w + x) * 3];
        p[0] = r, p[1] = g, p[2] = b;
    }
    RgbView view() const { return {px.data(), w, h, w * 3}; }
};
Zone redZone(float tol = 10) {
    Zone z;
    z.colors.push_back({"red", 0xFF, 0x3A, 0x45, tol});
    z.minPixels = 3;
    return z;
}

struct Log {
    std::vector<std::string> ev;
    bool present = false;
    MacroSinks sinks() {
        return {[this](int a, int f, Vec2 p) { ev.push_back("t" + std::to_string(a) + "f" + std::to_string(f) + "@" + std::to_string(int(p.x * 100))); },
                [this](int vk, bool d) { ev.push_back(std::string(d ? "kd" : "ku") + char(vk)); },
                [this](int) { return std::optional<bool>(present); }};
    }
};
Step wait(int ms) {
    Step s;
    s.type = StepType::Wait;
    s.ms = ms;
    return s;
}
Step tap(float x) {
    Step s;
    s.type = StepType::Tap;
    s.a = {x, 0.5f};
    return s;
}
void runFor(MacroRunner& r, int64_t& now, int64_t untilUs) {
    while (now < untilUs && r.active()) now += 1000, r.tick(now);
}
}  // namespace

TEST_CASE("Lab distance: identical colors match, distinct colors do not") {
    CHECK(deltaE(rgbToLab(255, 58, 69), rgbToLab(255, 58, 69)) == doctest::Approx(0));
    CHECK(deltaE(rgbToLab(255, 58, 69), rgbToLab(240, 50, 62)) < 10);
    CHECK(deltaE(rgbToLab(255, 58, 69), rgbToLab(40, 200, 60)) > 50);
}

TEST_CASE("only pixels inside the circle are counted") {
    Img img(40, 40, 0, 0, 0);
    // red pixels in the bounding-box corners (outside the circle)
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) img.set(x, y, 0xFF, 0x3A, 0x45), img.set(39 - x, 39 - y, 0xFF, 0x3A, 0x45);
    const ZoneMatch outside = matchZone(img.view(), 20, 20, 20, redZone());
    CHECK(outside.perColor[0] == 0);
    CHECK_FALSE(outside.present);
    CHECK(outside.total > 1200);
    CHECK(outside.total < 1300);  // ~pi*20^2
    for (int x = 18; x < 22; ++x) img.set(x, 20, 0xFF, 0x3A, 0x45);
    const ZoneMatch inside = matchZone(img.view(), 20, 20, 20, redZone());
    CHECK(inside.perColor[0] == 4);
    CHECK(inside.present);
}

TEST_CASE("tolerance, minimum pixels and any/all modes") {
    Img img(20, 20, 10, 10, 10);
    for (int x = 5; x < 15; ++x) img.set(x, 10, 240, 50, 62);  // slightly different red
    CHECK_FALSE(matchZone(img.view(), 10, 10, 9, redZone(2)).present);
    CHECK(matchZone(img.view(), 10, 10, 9, redZone(12)).present);
    Zone z = redZone(12);
    z.minPixels = 50;
    CHECK_FALSE(matchZone(img.view(), 10, 10, 9, z).present);
    z = redZone(12);
    z.colors.push_back({"green", 0, 200, 0, 10});
    CHECK(matchZone(img.view(), 10, 10, 9, z).present);
    z.mode = ColorMatchMode::All;
    CHECK_FALSE(matchZone(img.view(), 10, 10, 9, z).present);
    img.set(10, 12, 0, 200, 0), img.set(11, 12, 0, 200, 0), img.set(12, 12, 0, 200, 0);
    CHECK(matchZone(img.view(), 10, 10, 9, z).present);
}

TEST_CASE("monitor rearm: fires once per appearance, with debounce") {
    Monitor m;
    m.mode = TriggerMode::Rearm;
    m.confirm = 2;
    m.cooldownMs = 0;
    MonitorRuntime rt;
    int64_t t = 0;
    auto s = [&](bool d) { return rt.onSample(d, t += 20000, m, false); };
    CHECK_FALSE(s(true));  // 1st hit: not confirmed yet
    CHECK(s(true));        // confirmed -> fire
    CHECK_FALSE(s(true));  // still visible: no repeat
    CHECK_FALSE(s(false));
    CHECK_FALSE(s(true));  // single flicker does not count as disappearance
    CHECK_FALSE(s(false));
    CHECK_FALSE(s(false));  // gone (confirmed)
    CHECK_FALSE(s(true));
    CHECK(s(true));  // reappeared -> fire again
    CHECK(rt.runs() == 2);
}

TEST_CASE("monitor continuous respects cooldown; limited stops; busy action is not re-entered") {
    Monitor m;
    m.mode = TriggerMode::Continuous;
    m.confirm = 1;
    m.cooldownMs = 100;
    MonitorRuntime rt;
    int fired = 0;
    for (int64_t t = 0; t < 1000000; t += 20000) fired += rt.onSample(true, t, m, false);
    CHECK(fired >= 9);
    CHECK(fired <= 11);
    m.mode = TriggerMode::Limited;
    m.maxRuns = 3;
    m.cooldownMs = 0;
    rt.reset();
    fired = 0;
    for (int64_t t = 0; t < 1000000; t += 20000) fired += rt.onSample(true, t, m, false);
    CHECK(fired == 3);
    CHECK(rt.finished());
    m.mode = TriggerMode::Continuous;
    rt.reset();
    fired = 0;
    for (int64_t t = 0; t < 1000000; t += 20000) fired += rt.onSample(true, t, m, true);
    CHECK(fired == 0);
}

TEST_CASE("macro runs steps in order with non-blocking waits") {
    Automation a;
    Macro mac;
    Step press;
    press.type = StepType::PressDown;
    press.a = {0.2f, 0.5f};
    Step swipe;
    swipe.type = StepType::Swipe;
    swipe.a = {0.2f, 0.5f};
    swipe.b = {0.8f, 0.5f};
    swipe.ms = 250;
    Step rel;
    rel.type = StepType::Release;
    Step key;
    key.type = StepType::Key;
    key.key = 'R';
    mac.steps = {wait(200), tap(0.1f), wait(300), press, swipe, rel, wait(150), key, wait(200)};
    a.macros.push_back(mac);
    Log log;
    MacroRunner r(log.sinks());
    int64_t now = 0;
    REQUIRE(r.start(a, 0, now));
    r.tick(now);
    CHECK(log.ev.empty());  // first wait
    runFor(r, now, 199000);
    CHECK(log.ev.empty());
    runFor(r, now, 10000000);
    CHECK(r.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 7);
    CHECK(log.ev[0] == "t0f0@10");
    CHECK(log.ev[1] == "t1f0@10");
    CHECK(log.ev[2] == "t0f0@20");                  // press B
    CHECK(log.ev[log.ev.size() - 3] == "t1f0@80");  // release at C after the swipe
    CHECK(log.ev[log.ev.size() - 2] == "kdR");
    CHECK(log.ev.back() == "kuR");
    CHECK(now >= 1100000);  // total timing respected (200+40+300+250+150+40+200)
}

TEST_CASE("if/else takes exactly one branch; loop count; break") {
    Automation a;
    Macro mac;
    Step cond;
    cond.type = StepType::If;
    cond.zone = 0;
    cond.then = {tap(0.1f)};
    cond.otherwise = {tap(0.9f)};
    Step loop;
    loop.type = StepType::Loop;
    loop.count = 3;
    loop.ms = 0;
    loop.then = {tap(0.5f)};
    mac.steps = {cond, loop};
    a.zones.push_back(Zone{});
    a.macros.push_back(mac);
    for (bool present : {true, false}) {
        Log log;
        log.present = present;
        MacroRunner r(log.sinks());
        int64_t now = 0;
        r.start(a, 0, now);
        runFor(r, now, 5000000);
        CHECK(r.state() == RunState::Completed);
        REQUIRE(log.ev.size() == 8);
        CHECK(log.ev[0] == (present ? "t0f0@10" : "t0f0@90"));
        CHECK(log.ev[2] == "t0f0@50");
        CHECK(log.ev[6] == "t0f0@50");
    }
    Step brk;
    brk.type = StepType::Break;
    a.macros[0].steps[1].then = {tap(0.5f), brk, tap(0.7f)};
    Log log;
    MacroRunner r(log.sinks());
    int64_t now = 0;
    r.start(a, 0, now);
    runFor(r, now, 5000000);
    CHECK(log.ev.size() == 4);  // if-branch tap + one loop tap, then break
}

TEST_CASE("cancel releases held touches and keys; time limit stops endless loops") {
    Automation a;
    Macro mac;
    Step press;
    press.type = StepType::PressDown;
    Step key;
    key.type = StepType::Key;
    key.key = 'W';
    key.keyMode = KeyMode::Down;
    Step loop;
    loop.type = StepType::Loop;
    loop.loopMode = LoopMode::While;
    loop.zone = 0;
    loop.count = kMaxLoopCount;
    loop.ms = 10;
    mac.steps = {press, key, loop};
    mac.maxDurationMs = 500;
    a.zones.push_back(Zone{});
    a.macros.push_back(mac);
    Log log;
    log.present = true;
    MacroRunner r(log.sinks());
    int64_t now = 0;
    r.start(a, 0, now);
    runFor(r, now, 100000);
    CHECK(r.active());
    r.cancel();
    CHECK(r.state() == RunState::Cancelled);
    CHECK(log.ev[log.ev.size() - 2].rfind("t1f0", 0) == 0);
    CHECK(log.ev.back() == "kuW");
    log.ev.clear();
    r.start(a, 0, now);
    runFor(r, now, now + 2000000);
    CHECK(r.state() == RunState::Failed);
    CHECK(log.ev.back() == "kuW");
}

TEST_CASE("missing frame fails the color check safely") {
    Automation a;
    Macro mac;
    Step cond;
    cond.type = StepType::If;
    cond.zone = 0;
    mac.steps = {cond};
    a.zones.push_back(Zone{});
    a.macros.push_back(mac);
    MacroRunner r({nullptr, nullptr, [](int) { return std::optional<bool>(); }});
    r.start(a, 0, 0);
    r.tick(1000);
    CHECK(r.state() == RunState::Failed);
}

TEST_CASE("automation round-trips inside a profile; old profiles stay unchanged") {
    Profile p = makeDefaultProfile();
    const std::string plain = profileToJson(p);
    CHECK(plain.find("automation") == std::string::npos);
    Zone z = redZone();
    z.center = {0.3f, 0.7f};
    p.automation.zones.push_back(z);
    Macro m;
    m.name = "Reload";
    m.key = 'G';
    Step cond;
    cond.type = StepType::If;
    cond.zone = 0;
    cond.then = {wait(100), tap(0.4f)};
    cond.otherwise = {wait(150)};
    m.steps = {cond};
    p.automation.macros.push_back(m);
    Monitor mon;
    mon.zone = 0;
    mon.macro = 0;
    p.automation.monitors.push_back(mon);
    Profile q;
    std::string err;
    REQUIRE(profileFromJson(profileToJson(p), q, err));
    CHECK(q == p);
    p.automation.macros[0].key = ' ';  // Space is the default Jump key
    CHECK_FALSE(validateProfile(p).empty());
}

TEST_CASE("monitor toggleKey round-trips through automation JSON") {
    Automation a;
    a.zones.push_back(redZone());
    Monitor m;
    m.zone = 0;
    m.key = 'F';
    m.toggleKey = 0x78;  // VK_F9
    a.monitors.push_back(m);
    Automation out;
    std::string err;
    REQUIRE(automationFromJson(automationToJson(a), out, err));
    REQUIRE(out.monitors.size() == 1);
    CHECK(out.monitors[0].toggleKey == 0x78);
    // A field written by an older version (absent) defaults to "no key".
    Automation old;
    REQUIRE(automationFromJson(
        R"({"zones":[{"name":"Z","center":[0.5,0.5],"radius":0.05,"colors":[{"name":"r","rgb":[255,0,0],"tolerance":10}],"mode":"any","minPixels":3,"minRatio":0}],"monitors":[{"zone":0,"key":70}]})",
        old, err));
    REQUIRE(old.monitors.size() == 1);
    CHECK(old.monitors[0].toggleKey == 0);
}

TEST_CASE("detect watch aim-assist fields round-trip through automation JSON") {
    Automation a;
    a.detect.aimOn = true;
    a.detect.aimKey = 0x72;  // VK_F3
    a.detect.aimSpeedPct = 70;
    a.detect.aimRangePct = 55;
    a.detect.aimLeadMs = 120;
    a.detect.aimBonePct = 25;
    a.detect.aimTargetMode = 1;
    Automation out;
    std::string err;
    REQUIRE(automationFromJson(automationToJson(a), out, err));
    CHECK(out.detect.aimOn);
    CHECK(out.detect.aimKey == 0x72);
    CHECK(out.detect.aimSpeedPct == 70);
    CHECK(out.detect.aimRangePct == 55);
    CHECK(out.detect.aimLeadMs == 120);
    CHECK(out.detect.aimBonePct == 25);
    CHECK(out.detect.aimTargetMode == 1);
    // Older profiles without the fields load with safe defaults (assist off).
    Automation old;
    REQUIRE(automationFromJson(R"({"detect":{"startKey":100}})", old, err));
    CHECK_FALSE(old.detect.aimOn);
    CHECK(old.detect.aimKey == 0);
    CHECK(old.detect.aimSpeedPct == 50);
}

TEST_CASE("monitor runtime: switchedOn gate is independent of detection logic") {
    MonitorRuntime rt;
    Monitor m;
    m.zone = 0;
    m.key = 'F';
    m.mode = TriggerMode::Continuous;
    m.cooldownMs = 0;
    m.confirm = 1;
    // Detection logic is unchanged by the switch — the caller gates onSample.
    CHECK(rt.onSample(true, 1000000, m, false));
    // Re-arming resets hit counters so a stale burst cannot fire instantly.
    rt.reset();
    rt.switchedOn = true;
    CHECK(rt.switchedOn);
    CHECK(rt.runs() == 0);
    CHECK_FALSE(rt.present());
}

TEST_CASE("monitor else-branch: absence timer, cooldown, detection resets clock") {
    MonitorRuntime rt;
    Monitor m;
    m.confirm = 1;
    m.absentMs = 100;
    m.elseCooldownMs = 50;
    // No else configured → never fires.
    CHECK_FALSE(rt.onAbsent(0, m));
    m.elseKey = 'E';
    int64_t t = 0;
    CHECK_FALSE(rt.onAbsent(t, m));        // first absent sample starts the clock
    t += 200000;                          // 200ms ≥ absentMs → fire
    CHECK(rt.onAbsent(t, m));
    t += 20000;                           // 20ms < elseCooldownMs → hold
    CHECK_FALSE(rt.onAbsent(t, m));
    t += 40000;                           // 60ms > cooldown → fire again
    CHECK(rt.onAbsent(t, m));
    // Confirmed detection suppresses the else branch.
    rt.onSample(true, t + 1, m, false);
    CHECK(rt.present());
    CHECK_FALSE(rt.onAbsent(t + 2, m));

    // elseStop alone counts as a configured branch (the caller disarms).
    MonitorRuntime rt2;
    Monitor m2;
    m2.elseStop = true;
    m2.absentMs = 0;
    CHECK(rt2.onAbsent(0, m2));
}

TEST_CASE("monitor else-branch fields round-trip through automation JSON") {
    Automation a;
    a.zones.push_back(redZone());
    Monitor m;
    m.zone = 0;
    m.key = 'F';
    m.elseKey = 'E';
    m.absentMs = 1500;
    m.elseCooldownMs = 750;
    a.monitors.push_back(m);
    Automation out;
    std::string err;
    REQUIRE(automationFromJson(automationToJson(a), out, err));
    REQUIRE(out.monitors.size() == 1);
    CHECK(out.monitors[0].elseKey == 'E');
    CHECK(out.monitors[0].absentMs == 1500);
    CHECK(out.monitors[0].elseCooldownMs == 750);
    CHECK_FALSE(out.monitors[0].elseStop);
}

TEST_CASE("invalid automation data is rejected") {
    Automation a;
    a.zones.push_back(Zone{});
    a.zones[0].radius = 2;
    CHECK_FALSE(validateAutomation(a).empty());
    Automation b;
    Macro m;
    Step s;
    s.type = StepType::RunMacro;
    s.macro = 7;
    m.steps = {s};
    b.macros.push_back(m);
    CHECK_FALSE(validateAutomation(b).empty());
    Automation out;
    std::string err;
    CHECK_FALSE(automationFromJson(R"({"macros":[{"steps":[{"type":99}]}]})", out, err));
}
