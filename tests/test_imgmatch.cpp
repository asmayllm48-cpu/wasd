#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/automation.h"
#include "core/imgmatch.h"

using namespace wasd;

namespace {
// A deterministic pseudo-random hash texture so NCC has texture to lock on to
// (different seeds are truly uncorrelated, unlike a linear modulo pattern).
uint8_t pat(int x, int y, int s) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(s) * 974711u;
    h ^= h >> 13, h *= 1274126177u, h ^= h >> 16;
    return uint8_t(h % 251u + 4);
}

// RGB24 frame helper (matching the FrameView layout the matcher consumes).
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
};

Img pasteAt(int fw, int fh, int w, int h, int x0, int y0, int seed, const uint8_t* src = nullptr, int sw = 0) {
    Img f(fw, fh, 120, 120, 120);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v[3];
            if (src) {  // copy from the given RGB buffer
                const uint8_t* p = src + (size_t(y) * sw + x) * 3;
                v[0] = p[0], v[1] = p[1], v[2] = p[2];
            } else {
                v[0] = pat(x + seed, y + seed, seed);
                v[1] = uint8_t(v[0] / 2 + 80);
                v[2] = uint8_t(255 - v[0]);
            }
            f.set(x0 + x, y0 + y, v[0], v[1], v[2]);
        }
    return f;
}

ImageRef makeRef(int w, int h) {
    ImageRef r;
    r.w = uint32_t(w), r.h = uint32_t(h);
    r.rgb.resize(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t a = pat(x, y, 5), b = uint8_t(a / 2 + 80), c = uint8_t(255 - a);
            uint8_t* p = &r.rgb[(size_t(y) * w + x) * 3];
            p[0] = a, p[1] = b, p[2] = c;
        }
    r.thresholdPct = 85;
    r.region = Rect{0.0f, 0.0f, 1.0f, 1.0f};
    return r;
}

Step wait(int ms) {
    Step s;
    s.type = StepType::Wait;
    s.ms = ms;
    return s;
}

// Low-frequency reference content (like real UI buttons); survives resampling.
float smoothPat(int x, int y) {
    return std::clamp(128.0f + 50.0f * std::sin(x * 0.35f) + 40.0f * std::cos(y * 0.5f) +
                          90.0f * std::sin((x + y) * 0.12f) - 0.0f * 0,
                      4.0f, 251.0f);
}
ImageRef makeSmoothRef(int w, int h) {
    ImageRef r;
    r.w = uint32_t(w), r.h = uint32_t(h);
    r.rgb.resize(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float a = smoothPat(x, y), b = std::clamp(a / 2 + 80, 4.0f, 251.0f), c = std::clamp(255 - a, 4.0f, 251.0f);
            uint8_t* p = &r.rgb[(size_t(y) * w + x) * 3];
            p[0] = uint8_t(a), p[1] = uint8_t(b), p[2] = uint8_t(c);
        }
    r.thresholdPct = 85;
    r.region = Rect{0.0f, 0.0f, 1.0f, 1.0f};
    return r;
}
}  // namespace

TEST_CASE("exact crop is found at its location with a high score") {
    const ImageRef ref = makeRef(40, 24);
    Img f = pasteAt(320, 240, 40, 24, 100, 60, 5, ref.rgb.data(), int(ref.w));
    const ImageMatch m = matchImage(FrameView{f.px.data(), f.w, f.h, f.w * 3}, ref);
    CHECK(m.found);
    CHECK(m.score > 0.97f);
    CHECK(m.center.x == doctest::Approx(120.0f / 320.0f).epsilon(0.02));
    CHECK(m.center.y == doctest::Approx(72.0f / 240.0f).epsilon(0.02));
}

TEST_CASE("a different screen is not a match at the default threshold") {
    const ImageRef ref = makeRef(40, 24);
    Img f = pasteAt(320, 240, 40, 24, 100, 60, 99);  // another texture
    const ImageMatch m = matchImage(FrameView{f.px.data(), f.w, f.h, f.w * 3}, ref);
    CHECK_FALSE(m.found);
}

TEST_CASE("search region excludes the rest of the frame") {
    ImageRef ref = makeRef(40, 24);
    ref.region = Rect{0.0f, 0.0f, 0.3f, 1.0f};  // the crop sits at x=100+ px
    Img f = pasteAt(320, 240, 40, 24, 200, 80, 5, ref.rgb.data(), int(ref.w));
    const ImageMatch m = matchImage(FrameView{f.px.data(), f.w, f.h, f.w * 3}, ref);
    CHECK_FALSE(m.found);
}

TEST_CASE("a 10 percent larger appearance is still found (multi-scale)") {
    const ImageRef ref = makeSmoothRef(40, 24);  // smooth content, like real UI
    Img f(320, 240, 120, 120, 120);
    // render the reference enlarged by 1.1x
    const int dw = 44, dh = 27;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            const int sx = int(float(x) / 44.0f * 40), sy = int(float(y) / 27.0f * 24);
            const uint8_t* p = &ref.rgb[(size_t(sy) * ref.w + sx) * 3];
            f.set(140 + x, 70 + y, p[0], p[1], p[2]);
        }
    const ImageMatch m = matchImage(FrameView{f.px.data(), f.w, f.h, f.w * 3}, ref);
    CHECK(m.found);
    CHECK(m.center.x > doctest::Approx(140.0f / 320.0f - 0.02));
    CHECK(m.center.x < doctest::Approx(184.0f / 320.0f + 0.02));
}

TEST_CASE("brightness shift keeps the match (NCC is brightness-robust)") {
    ImageRef ref = makeRef(40, 24);
    Img f(320, 240, 200, 200, 200);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 40; ++x) {
            const uint8_t* p = &ref.rgb[(size_t(y) * ref.w + x) * 3];
            f.set(60 + x, 40 + y, uint8_t(std::min(255, p[0] + 40)), uint8_t(std::min(255, p[1] + 40)),
                  uint8_t(std::min(255, p[2] + 40)));
        }
    const ImageMatch m = matchImage(FrameView{f.px.data(), f.w, f.h, f.w * 3}, ref);
    CHECK(m.found);
}

TEST_CASE("match in a clipped region view translates back to frame coordinates") {
    // Simulates the app path: the GPU readback contains only the search region,
    // the hit is found inside that view, then converted to frame-normalized coords.
    const ImageRef ref = makeRef(30, 20);
    Img f = pasteAt(320, 240, 30, 20, 170, 130, 5, ref.rgb.data(), int(ref.w));
    // Readback of region {0.25,0.25,0.5,0.5} -> pixels (80,60,160,120) of the frame.
    const int rx = 80, ry = 60, rw = 160, rh = 120;
    std::vector<uint8_t> region(size_t(rw) * rh * 3);
    for (int yy = 0; yy < rh; ++yy)
        for (int xx = 0; xx < rw; ++xx) {
            const uint8_t* p = &f.px[(size_t(ry + yy) * f.w + rx + xx) * 3];
            uint8_t* q = &region[(size_t(yy) * rw + xx) * 3];
            q[0] = p[0], q[1] = p[1], q[2] = p[2];
        }
    const ImageMatch inView = matchImageInRect(FrameView{region.data(), rw, rh, rw * 3}, ref, 0, 0, rw, rh);
    REQUIRE(inView.found);
    const ImageMatch m = toFrameCoords(inView, rx, ry, rw, rh, f.w, f.h);
    CHECK(m.rect.x == doctest::Approx(170.0f / 320.0f).epsilon(0.02));
    CHECK(m.rect.y == doctest::Approx(130.0f / 240.0f).epsilon(0.02));
    CHECK(m.center.x == doctest::Approx(185.0f / 320.0f).epsilon(0.02));
    CHECK(m.center.y == doctest::Approx(140.0f / 240.0f).epsilon(0.02));
}

TEST_CASE("image refs and steps round-trip through the automation JSON") {
    Automation a;
    ImageRef r;
    r.name = "confirm_button";
    r.w = 12, r.h = 8, r.rgb.resize(12 * 8 * 3, 7);
    r.thresholdPct = 70;
    r.region = Rect{0.1f, 0.2f, 0.5f, 0.6f};
    r.clickOffset = {0.25f, 0.75f};
    r.intervalMs = 200;
    r.file = "p1-123.wasdimg";
    a.images.push_back(r);
    Zone z;
    a.zones.push_back(z);
    Macro mac;
    mac.name = "demo";
    mac.key = 'X';
    Step search;
    search.type = StepType::ImageSearch;
    search.image = 0;
    search.ms = 3000;
    Step click;
    click.type = StepType::ClickImage;
    click.image = 0;
    Step iff;
    iff.type = StepType::If;
    iff.image = 0;
    iff.zone = -1;
    Step w = wait(100);
    iff.then.push_back(w);
    mac.steps = {search, click, iff};
    a.macros.push_back(mac);
    Monitor im;
    im.name = "imgmon";
    im.image = 0;
    im.zone = -1;
    im.macro = -1;
    im.key = 'X';
    a.monitors.push_back(im);

    const std::string text = automationToJson(a);
    Automation out;
    std::string err;
    REQUIRE_MESSAGE(automationFromJson(text, out, err), "parse error: " << err);
    REQUIRE(out.images.size() == 1);
    CHECK(out.images[0].name == r.name);
    CHECK(out.images[0].w == r.w);
    CHECK(out.images[0].h == r.h);
    CHECK(out.images[0].thresholdPct == 70);
    CHECK(out.images[0].region.x == doctest::Approx(r.region.x));
    CHECK(out.images[0].region.y == doctest::Approx(r.region.y));
    CHECK(out.images[0].region.w == doctest::Approx(r.region.w));
    CHECK(out.images[0].region.h == doctest::Approx(r.region.h));
    CHECK(out.images[0].clickOffset == r.clickOffset);
    CHECK(out.images[0].intervalMs == 200);
    CHECK(out.images[0].file == r.file);
    REQUIRE(out.macros.size() == 1);
    REQUIRE(out.macros[0].steps.size() == 3);
    // steps moved indexes survive (image references point at the fresh ImageRef list)
    CHECK(out.monitors.size() == 1);
    CHECK(out.monitors[0].image == 0);
    // deleting the image turns image conditions into validation errors with a clear message
    out.images.clear();
    out.zones.clear();
    const auto errs = validateAutomation(out);
    CHECK(errs.size() >= 4);
}

namespace {
struct ImgLog {
    std::vector<std::string> ev;
    bool present = true;
    Vec2 pos{0.5f, 0.4f};
    Rect rect{0.4f, 0.3f, 0.2f, 0.2f};
    float score = 0.9f;
    std::vector<ImageMatch> extra;  // additional candidates for multi-match tests
    MacroSinks sinks() {
        MacroSinks s;
        s.touch = [this](int a, int f, Vec2 p) {
            ev.push_back("t" + std::to_string(a) + " f" + std::to_string(f) + "@" + std::to_string(int(p.x * 1000)) + "," +
                         std::to_string(int(p.y * 1000)));
        };
        s.key = [this](int vk, bool d) { ev.push_back(std::string(d ? "kd" : "ku") + char(vk)); };
        s.zonePresent = [this](int) { return std::optional<bool>(false); };
        s.imagePresent = [this](int) { return std::optional<bool>(present); };
        s.imageScan = [this](int, const ImageQuery&) {
            ImageScanResult r;
            r.available = true;
            r.paramsMatch = true;
            r.found = present;
            r.score = score;
            if (present) {
                ImageMatch m;
                m.found = true;
                m.center = pos;
                m.rect = rect;
                m.score = score;
                r.match = m;
                r.matches.push_back(m);
                for (const ImageMatch& e : extra) r.matches.push_back(e);
            }
            return r;
        };
        return s;
    }
};
Step imageStep(int timeoutMs = 1000) {
    Step s;
    s.type = StepType::ImageRecognize;
    s.image = 0;
    s.ms = timeoutMs;
    return s;
}
Step tapAt(float x) {
    Step s;
    s.type = StepType::Tap;
    s.a = {x, 0.5f};
    return s;
}
void runFor(MacroRunner& r, int64_t& now, int64_t us) {
    for (int64_t t = 0; t < us && r.active(); t += 1000) now += 1000, r.tick(now);
}
}  // namespace

TEST_CASE("ImageRecognize: found -> click the detected bbox center, then continue") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::ClickMatch;  // clickOfs default = center
    mac.steps = {s, tapAt(0.1f)};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 1000000;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 4);
    // bbox {0.4,0.3,0.2,0.2} + center offset -> (0.5, 0.4); then the next tap at x=0.1
    CHECK(log.ev[0] == "t0 f0@500,400");
    CHECK(log.ev[1] == "t1 f0@500,400");
    CHECK(log.ev[2] == "t0 f0@100,500");
}

TEST_CASE("ImageRecognize: custom offset inside the detected bbox + press duration") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::ClickMatch;
    s.clickOfs = {0.25f, 0.75f};  // -> (0.4+0.05, 0.3+0.15) = (0.45, 0.45)
    s.clickKind = ClickKind::LongPress;
    s.pressMs = 120;
    mac.steps = {s};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 1000000;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 300000);
    CHECK(log.ev[0] == "t0 f0@450,450");
    // the finger stays down for ~120 ms before the release
    int64_t upAt = -1;
    for (size_t i = 0; i < log.ev.size(); ++i)
        if (log.ev[i] == "t1 f0@450,450") upAt = int64_t(i);
    CHECK(upAt >= 0);
    CHECK(now - 1000000 >= 120000);
    CHECK(runner.state() == RunState::Completed);
}

TEST_CASE("ImageRecognize: double tap emits two down/up pairs") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::ClickMatch;
    s.clickKind = ClickKind::DoubleTap;
    mac.steps = {s};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 1000000;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    int downs = 0, ups = 0;
    for (const auto& e : log.ev) downs += e.rfind("t0 f0", 0) == 0, ups += e.rfind("t1 f0", 0) == 0;
    CHECK(downs == 2);
    CHECK(ups == 2);
}

TEST_CASE("ImageRecognize: found -> run the ON FOUND step sequence") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::RunSteps;
    s.then = {tapAt(0.2f), tapAt(0.8f)};
    mac.steps = {s};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 1000000;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 4);
    CHECK(log.ev[0] == "t0 f0@200,500");
    CHECK(log.ev[2] == "t0 f0@800,500");
}

TEST_CASE("ImageRecognize: found -> click, THEN run the follow-up sequence") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::ClickMatch;
    s.then = {tapAt(0.7f)};  // runs AFTER the click
    mac.steps = {s};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 0;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 4);
    CHECK(log.ev[0] == "t0 f0@500,400");  // the click first...
    CHECK(log.ev[1] == "t1 f0@500,400");
    CHECK(log.ev[2] == "t0 f0@700,500");  // ...then the follow-up sequence
}

TEST_CASE("ImageRecognize: jump to a selected step on found") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::JumpStep;
    s.jumpTo = 2;  // skip the middle step
    mac.steps = {s, tapAt(0.1f), tapAt(0.9f)};
    a.macros.push_back(mac);
    ImgLog log;
    MacroRunner runner(log.sinks());
    int64_t now = 1000000;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 2);
    CHECK(log.ev[0] == "t0 f0@900,500");  // only step 3 ran
}

TEST_CASE("ImageRecognize: not found -> continue / alternative sequence / fail") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);

    // continue to the next step
    {
        Macro mac;
        Step s = imageStep(150);
        s.notFoundAction = ImageNotFoundAction::Continue;
        mac.steps = {s, tapAt(0.3f)};
        a.macros = {mac};
        ImgLog log;
        log.present = false;
        MacroRunner runner(log.sinks());
        int64_t now = 0;
        REQUIRE(runner.start(a, 0, now));
        runFor(runner, now, 500000);
        CHECK(runner.state() == RunState::Completed);
        REQUIRE(log.ev.size() >= 2);
        CHECK(log.ev[0] == "t0 f0@300,500");
    }
    // run the ON NOT FOUND sequence
    {
        Macro mac;
        Step s = imageStep(150);
        s.notFoundAction = ImageNotFoundAction::RunSteps;
        s.otherwise = {tapAt(0.7f)};
        mac.steps = {s};
        a.macros = {mac};
        ImgLog log;
        log.present = false;
        MacroRunner runner(log.sinks());
        int64_t now = 0;
        REQUIRE(runner.start(a, 0, now));
        runFor(runner, now, 500000);
        CHECK(runner.state() == RunState::Completed);
        REQUIRE(log.ev.size() >= 2);
        CHECK(log.ev[0] == "t0 f0@700,500");
    }
    // trigger an error/failure result on timeout
    {
        Macro mac;
        Step s = imageStep(150);
        s.notFoundAction = ImageNotFoundAction::Fail;
        mac.steps = {s};
        a.macros = {mac};
        ImgLog log;
        log.present = false;
        MacroRunner runner(log.sinks());
        int64_t now = 0;
        REQUIRE(runner.start(a, 0, now));
        runFor(runner, now, 500000);
        CHECK(runner.state() == RunState::Failed);
        CHECK(log.ev.empty());
    }
}

TEST_CASE("ImageRecognize: retry count bounds the search before the timeout") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep(60000);       // generous timeout...
    s.retryCount = 3;                 // ...but only 3 missed scans allowed
    s.retryIntervalMs = 10;
    s.notFoundAction = ImageNotFoundAction::Stop;
    mac.steps = {s, tapAt(0.3f)};
    a.macros.push_back(mac);
    ImgLog log;
    log.present = false;
    MacroRunner runner(log.sinks());
    int64_t now = 0;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 5000000);
    CHECK(runner.state() == RunState::Completed);  // Stop = completed
    CHECK(now < 60000000);                          // long before the timeout
    CHECK(log.ev.empty());
}

TEST_CASE("ImageRecognize: cancelling mid-search stops cleanly with no stale clicks") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    mac.steps = {imageStep(60000)};
    a.macros.push_back(mac);
    ImgLog log;
    log.present = false;
    MacroRunner runner(log.sinks());
    int64_t now = 0;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 100000);
    CHECK(runner.state() == RunState::Waiting);
    runner.cancel();
    CHECK(runner.state() == RunState::Cancelled);
    log.present = true;
    runFor(runner, now, 200000);
    CHECK(log.ev.empty());  // no click fired after the cancel
}

TEST_CASE("ImageRecognize: Nearest policy picks the closest candidate") {
    Automation a;
    ImageRef r;
    r.w = 8, r.h = 6, r.rgb.resize(8 * 6 * 3, 3);
    a.images.push_back(r);
    Macro mac;
    Step s = imageStep();
    s.foundAction = ImageFoundAction::ClickMatch;
    s.matchPolicy = MatchPolicy::Nearest;
    s.nearTo = {0.9f, 0.9f};
    mac.steps = {s};
    a.macros.push_back(mac);
    ImgLog log;
    ImageMatch other;  // second candidate closer to (0.9, 0.9)
    other.found = true;
    other.rect = Rect{0.7f, 0.7f, 0.2f, 0.2f};
    other.center = {0.8f, 0.8f};
    log.extra.push_back(other);
    MacroRunner runner(log.sinks());
    int64_t now = 0;
    REQUIRE(runner.start(a, 0, now));
    runFor(runner, now, 500000);
    CHECK(runner.state() == RunState::Completed);
    REQUIRE(log.ev.size() >= 2);
    CHECK(log.ev[0] == "t0 f0@800,800");  // clicked the nearer candidate, not the best
}

TEST_CASE("ImageRecognize: legacy Image search and Click image steps migrate on load") {
    // A profile JSON written by the old build (type 13 = Image search, 14 = Click image).
    const std::string legacy = R"({
        "images": [{"name": "btn", "w": 8, "h": 6, "threshold": 80,
                    "region": [0.1, 0.1, 0.5, 0.5], "clickOffset": [0.25, 0.75],
                    "intervalMs": 100, "file": "x.wasdimg"}],
        "macros": [{"name": "old", "steps": [
            {"type": 13, "image": 0, "ms": 3000, "foundAction": 1,
             "foundClick": [0.6, 0.4], "notFoundAction": 0},
            {"type": 14, "image": 0, "ms": 200, "finger": 2}
        ]}]
    })";
    Automation a;
    std::string err;
    std::vector<std::string> notes;
    REQUIRE_MESSAGE(automationFromJson(legacy, a, err, &notes), "parse error: " << err);
    REQUIRE(a.macros.size() == 1);
    REQUIRE(a.macros[0].steps.size() == 2);
    const Step& search = a.macros[0].steps[0];
    CHECK(search.type == StepType::ImageRecognize);
    CHECK(search.image == 0);
    CHECK(search.ms == 3000);                                    // timeout preserved
    CHECK(search.foundAction == ImageFoundAction::ClickPoint);   // legacy index 1 remapped
    CHECK(search.foundClick == Vec2{0.6f, 0.4f});
    CHECK(search.notFoundAction == ImageNotFoundAction::Fail);   // legacy "Retry" failed on timeout
    const Step& click = a.macros[0].steps[1];
    CHECK(click.type == StepType::ImageRecognize);
    CHECK(click.foundAction == ImageFoundAction::ClickMatch);
    CHECK(click.notFoundAction == ImageNotFoundAction::Fail);    // it failed when not seen
    CHECK(click.ms == 0);                                        // it never waited
    CHECK(click.clickOfs == Vec2{0.25f, 0.75f});                 // image's click offset preserved
    CHECK(click.clickKind == ClickKind::LongPress);              // 200 ms hold -> long press
    CHECK(click.pressMs == 200);
    CHECK(click.finger == 2);
    CHECK(notes.size() == 2);                                    // both migrations reported
    // The migrated profile re-serializes as the unified type only.
    const std::string text = automationToJson(a);
    CHECK(text.find("\"type\":13") == std::string::npos);
    CHECK(text.find("\"type\":14") == std::string::npos);
    Automation a2;
    REQUIRE(automationFromJson(text, a2, err));
    CHECK(a2.macros[0].steps == a.macros[0].steps);
}
