#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "core/studio_engine.h"

using namespace wasd;
using namespace wasd::studio;

namespace {

struct FakeHost {
    std::vector<std::tuple<int, int, Vec2>> touches;  // action, finger, point
    std::vector<std::pair<int, bool>> keys;
    std::vector<std::string> logs;
    int ranMacro = -1;
    bool zone = false;
    ImageScanResult scan{};
    int pixelQueries = 0;
    ColorVal pixel{10, 200, 30, 255, 12.0f};
    int pixelDelay = 0;  // ticks to simulate async readback latency
    ColorScanResult colorScan{};

    GraphHost host() {
        GraphHost h;
        h.touch = [this](int a, int f, Vec2 p) { touches.emplace_back(a, f, p); };
        h.key = [this](int vk, bool d) { keys.emplace_back(vk, d); };
        h.zonePresent = [this](int) { return std::optional<bool>(zone); };
        h.imagePresent = [this](int) { return std::optional<bool>(false); };
        h.imageScan = [this](int, const ImageQuery&) { return scan; };
        h.runMacro = [this](int i) { ranMacro = i; };
        h.log = [this](int, const std::string& m) { logs.push_back(m); };
        h.pixelColor = [this](Vec2) -> std::optional<ColorVal> {
            return ++pixelQueries > pixelDelay ? std::optional<ColorVal>(pixel) : std::nullopt;
        };
        h.colorScan = [this](const Rect&, const ColorVal&) { return std::optional<ColorScanResult>(colorScan); };
        return h;
    }
};

// addNode returns a reference that dangling-references after vector growth —
// always work with ids and look nodes up through findNode.
uint32_t add(AutomationGraph& g, NodeType t, float x, float y = 0.0f) {
    return g.addNode(t, {x, y}).id;
}
GraphNode& node(AutomationGraph& g, uint32_t id) { return *g.findNode(id); }

void link(AutomationGraph& g, uint32_t a, const char* out, uint32_t b, const char* in = "in") {
    auto chk = g.connectByKey(a, out, b, in);
    INFO("link failed: ", chk.reason);
    REQUIRE(chk.ok);
}

// Convenience: run the runner until it leaves active() or us exceeds `limit`.
void runUntilDone(GraphRunner& r, int64_t us, int64_t stepUs = 5000, int64_t limit = 60000000) {
    while (r.active() && us < limit) {
        r.tick(us);
        us += stepUs;
    }
}

uint32_t findStart(const AutomationGraph& g) {
    for (const auto& n : g.nodes)
        if (n.type == NodeType::Start) return n.id;
    return 0;
}

}  // namespace

TEST_CASE("engine: linear Start->Click->End executes and completes") {
    Automation a;
    AutomationGraph g = makeStarterGraph("Linear");
    uint32_t s = findStart(g);
    REQUIRE(s != 0);

    uint32_t c = add(g, NodeType::Click, 300);
    node(g, c).props["point"] = Value::makeCoord({0.4f, 0.6f});
    node(g, c).props["holdMs"] = Value::makeInt(20);
    uint32_t e = add(g, NodeType::End, 600);
    link(g, s, "out", c);
    link(g, c, "out", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 1000000));
    runUntilDone(r, 1000000);
    CHECK(r.state() == RunState::Completed);
    REQUIRE(fh.touches.size() == 2);
    CHECK(std::get<0>(fh.touches[0]) == 0);  // down
    CHECK(std::get<0>(fh.touches[1]) == 1);  // up
    CHECK(std::get<2>(fh.touches[0]).x == doctest::Approx(0.4f));
}

TEST_CASE("engine: Delay waits non-blockingly and Wait respects ms") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t d = add(g, NodeType::Delay, 300);
    node(g, d).props["duration"] = Value::makeInt(100);
    uint32_t w = add(g, NodeType::Wait, 600);
    node(g, w).props["ms"] = Value::makeInt(50);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", d);
    link(g, d, "out", w);
    link(g, w, "out", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    r.tick(0);
    CHECK(r.state() == RunState::Waiting);  // inside Delay
    r.tick(50000);                           // 50 ms — still waiting
    CHECK(r.state() == RunState::Waiting);
    r.tick(100000);                          // 100 ms — delay done, enters Wait
    CHECK(r.state() == RunState::Waiting);
    r.tick(160000);                          // wait done -> End
    CHECK(r.state() == RunState::Completed);
}

TEST_CASE("engine: Loop iterates count times via implicit back-edge") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t lp = add(g, NodeType::Loop, 300);
    node(g, lp).props["count"] = Value::makeInt(3);
    uint32_t c = add(g, NodeType::Click, 600);
    node(g, c).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", lp);
    link(g, lp, "body", c);        // body ends at Click -> implicit back-edge
    link(g, lp, "completed", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    CHECK(fh.touches.size() == 6);  // 3 taps (down+up)
}

TEST_CASE("engine: If branches on wired condition and variables flow through math") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t cv = add(g, NodeType::CreateVariable, 200);
    node(g, cv).props["name"] = Value::makeString("n");
    node(g, cv).props["initial"] = Value::makeInt(4);   // prop fallback for unconnected pin
    uint32_t iff = add(g, NodeType::If, 400);
    uint32_t cT = add(g, NodeType::Click, 600);
    node(g, cT).props["point"] = Value::makeCoord({0.1f, 0.1f});
    node(g, cT).props["holdMs"] = Value::makeInt(15);
    uint32_t cF = add(g, NodeType::Click, 600, 200);
    node(g, cF).props["point"] = Value::makeCoord({0.9f, 0.9f});
    node(g, cF).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", cv);
    link(g, cv, "out", iff);
    link(g, iff, "true", cT);
    link(g, iff, "false", cF);
    link(g, cT, "out", e);
    link(g, cF, "out", e);

    // condition: (GetVariable n + 1) > 4  -> 5 > 4 -> true
    uint32_t gv = add(g, NodeType::GetVariable, 0, 100);
    node(g, gv).props["name"] = Value::makeString("n");
    uint32_t ad = add(g, NodeType::MathAdd, 150, 100);
    uint32_t gt = add(g, NodeType::GreaterThan, 300, 100);
    node(g, gt).props["b"] = Value::makeFloat(4.0);
    link(g, gv, "value", ad, "a");
    link(g, ad, "result", gt, "a");
    link(g, gt, "result", iff, "condition");

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    CHECK(r.vars().at("n").asInt() == 4);
    REQUIRE(fh.touches.size() == 2);
    CHECK(std::get<2>(fh.touches[0]).x == doctest::Approx(0.1f));  // true branch ran
}

TEST_CASE("engine: SetVariable + Break exits an infinite loop") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t lp = add(g, NodeType::Loop, 200);
    node(g, lp).props["infinite"] = Value::makeBool(true);
    uint32_t sv = add(g, NodeType::SetVariable, 400);
    node(g, sv).props["name"] = Value::makeString("i");
    uint32_t iff = add(g, NodeType::If, 600);
    uint32_t br = add(g, NodeType::Break, 800);
    uint32_t e = add(g, NodeType::End, 1000);
    link(g, s, "out", lp);
    link(g, lp, "body", sv);
    link(g, sv, "out", iff);
    link(g, iff, "true", br);   // If true -> Break -> loop completed -> End
    // loop index feeds "i"; condition: NOT(index < 2) = index >= 2
    uint32_t lt = add(g, NodeType::LessThan, 400, 150);
    node(g, lt).props["b"] = Value::makeFloat(2.0);
    link(g, lp, "index", sv, "value");
    link(g, lp, "index", lt, "a");
    uint32_t nt = add(g, NodeType::LogicNot, 600, 150);
    link(g, lt, "result", nt, "value");
    link(g, nt, "result", iff, "condition");
    link(g, lp, "completed", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    CHECK(r.vars().at("i").asInt() == 2);
}

TEST_CASE("engine: cancel releases held fingers and keys") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t pd = add(g, NodeType::PressDown, 300);
    node(g, pd).props["finger"] = Value::makeInt(2);
    uint32_t kp = add(g, NodeType::KeyPress, 500);
    node(g, kp).props["key"] = Value::makeInt(0x41);
    node(g, kp).props["holdOnly"] = Value::makeBool(true);
    uint32_t d = add(g, NodeType::Delay, 700);
    node(g, d).props["duration"] = Value::makeInt(60000);
    link(g, s, "out", pd);
    link(g, pd, "out", kp);
    link(g, kp, "out", d);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    r.tick(0);
    CHECK(r.state() == RunState::Waiting);
    REQUIRE(fh.touches.size() == 1);
    REQUIRE(fh.keys.size() == 1);
    r.cancel("test");
    CHECK(r.state() == RunState::Cancelled);
    CHECK(fh.touches.size() == 2);  // release emitted for finger 2
    CHECK(std::get<0>(fh.touches.back()) == 1);
    CHECK(std::get<1>(fh.touches.back()) == 2);
    CHECK(fh.keys.size() == 2);
    CHECK(fh.keys.back() == std::make_pair(0x41, false));
}

TEST_CASE("engine: step budget guards against runaway exec cycles") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t lp = add(g, NodeType::Loop, 300);
    node(g, lp).props["infinite"] = Value::makeBool(true);
    node(g, lp).props["pauseMs"] = Value::makeInt(0);
    uint32_t cm = add(g, NodeType::CommentNode, 600);
    link(g, s, "out", lp);
    link(g, lp, "body", cm);  // body dead-ends -> back-edge -> infinite
    g.metadata.maxStepsBudget = 500;

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0, 1000, 100000);
    CHECK(r.state() == RunState::Failed);
    CHECK(r.message().find("budget") != std::string::npos);
}

TEST_CASE("engine: breakpoint pauses, resume and step-over continue") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t c = add(g, NodeType::Click, 300);
    node(g, c).breakpoint = true;
    node(g, c).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 600);
    link(g, s, "out", c);
    link(g, c, "out", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    r.setDebug(true);
    REQUIRE(r.start(a, g, 0));
    r.tick(0);
    CHECK(r.paused());
    CHECK(r.state() == RunState::Running);
    CHECK(fh.touches.empty());
    r.resume();
    r.tick(1000);
    r.tick(50000);
    CHECK(r.state() == RunState::Completed);
    CHECK(r.nodeState(c) == NodeRunState::Done);
    CHECK(r.nodeState(s) == NodeRunState::Done);
    CHECK(!r.events().empty());
}

TEST_CASE("engine: Sequence executes then-branches in order") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t sq = add(g, NodeType::Sequence, 300);
    uint32_t c0 = add(g, NodeType::Click, 600);
    node(g, c0).props["point"] = Value::makeCoord({0.1f, 0.5f});
    node(g, c0).props["holdMs"] = Value::makeInt(15);
    uint32_t c1 = add(g, NodeType::Click, 700, 150);
    node(g, c1).props["point"] = Value::makeCoord({0.5f, 0.5f});
    node(g, c1).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", sq);
    link(g, sq, "then0", c0);
    link(g, sq, "then1", c1);
    link(g, c1, "out", e);
    // then0 branch ends at c0 with no wire -> sequence should advance to then1

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    REQUIRE(fh.touches.size() == 4);
    CHECK(std::get<2>(fh.touches[0]).x == doctest::Approx(0.1f));
    CHECK(std::get<2>(fh.touches[2]).x == doctest::Approx(0.5f));
}

TEST_CASE("engine: While and ForLoop honor iteration outputs") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t fl = add(g, NodeType::ForLoop, 300);
    node(g, fl).props["first"] = Value::makeInt(1);
    node(g, fl).props["last"] = Value::makeInt(3);
    uint32_t sv = add(g, NodeType::SetVariable, 600);
    node(g, sv).props["name"] = Value::makeString("i");
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", fl);
    link(g, fl, "body", sv);
    link(g, fl, "index", sv, "value");
    link(g, fl, "completed", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    CHECK(r.vars().at("i").asInt() == 3);
}

TEST_CASE("engine: FindImage polls the scan sink and branches on the result") {
    Automation a;
    a.images.push_back(ImageRef{});
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t fi = add(g, NodeType::FindImage, 300);
    node(g, fi).props["imageIndex"] = Value::makeInt(0);
    node(g, fi).props["timeoutMs"] = Value::makeInt(0);  // single check
    uint32_t cF = add(g, NodeType::Click, 600);
    node(g, cF).props["point"] = Value::makeCoord({0.7f, 0.7f});
    node(g, cF).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", fi);
    link(g, fi, "found_exec", cF);
    link(g, fi, "not_found_exec", e);
    link(g, cF, "out", e);

    SUBCASE("found -> found_exec runs click with result outputs") {
        FakeHost fh;
        fh.scan.available = true;
        fh.scan.paramsMatch = true;
        fh.scan.found = true;
        fh.scan.score = 0.93f;
        fh.scan.match.center = {0.33f, 0.44f};
        fh.scan.match.rect = {0.3f, 0.4f, 0.1f, 0.1f};
        fh.scan.scanUs = 4000;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        REQUIRE(fh.touches.size() == 2);
        const Value* out = r.nodeOutput(fi, "found");
        REQUIRE(out != nullptr);
        CHECK(out->asBool());
        CHECK(r.nodeOutput(fi, "similarity")->asFloat() == doctest::Approx(0.93));
        CHECK(r.nodeOutput(fi, "coordinates")->asCoord().x == doctest::Approx(0.33f));
    }
    SUBCASE("miss -> not_found_exec skips the click") {
        FakeHost fh;
        fh.scan.available = true;
        fh.scan.paramsMatch = true;
        fh.scan.found = false;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        CHECK(fh.touches.empty());
    }
}

TEST_CASE("engine: subgraph call executes and returns to caller") {
    Automation a;
    AutomationGraph g;
    AutomationGraph sub;
    sub.id = "sub1";
    sub.name = "MySub";
    uint32_t ss = add(sub, NodeType::Start, 0);
    uint32_t sc = add(sub, NodeType::Click, 300);
    node(sub, sc).props["point"] = Value::makeCoord({0.2f, 0.2f});
    node(sub, sc).props["holdMs"] = Value::makeInt(15);
    uint32_t se = add(sub, NodeType::End, 600);
    link(sub, ss, "out", sc);
    link(sub, sc, "out", se);
    g.subgraphs.push_back(sub);

    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t call = add(g, NodeType::Subgraph, 300);
    node(g, call).props["subgraphId"] = Value::makeString("sub1");
    uint32_t c2 = add(g, NodeType::Click, 600);
    node(g, c2).props["point"] = Value::makeCoord({0.8f, 0.8f});
    node(g, c2).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", call);
    link(g, call, "out", c2);
    link(g, c2, "out", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    REQUIRE(fh.touches.size() == 4);  // sub click + caller click
    CHECK(std::get<2>(fh.touches[0]).x == doctest::Approx(0.2f));
    CHECK(std::get<2>(fh.touches[2]).x == doctest::Approx(0.8f));
}

TEST_CASE("engine: GetPixelColor waits for the async probe and yields a Color") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t pc = add(g, NodeType::GetPixelColor, 300);
    node(g, pc).props["point"] = Value::makeCoord({0.5f, 0.25f});
    uint32_t e = add(g, NodeType::End, 600);
    link(g, s, "out", pc);
    link(g, pc, "out", e);

    FakeHost fh;
    fh.pixelDelay = 2;  // probe answers on the 3rd poll
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    r.tick(0);
    CHECK(r.state() == RunState::Waiting);  // readback pending
    runUntilDone(r, 1000);
    CHECK(r.state() == RunState::Completed);
    const Value* c = r.nodeOutput(pc, "color");
    REQUIRE(c != nullptr);
    CHECK(c->color.g == 200);
}

TEST_CASE("engine: FindColor region scan waits and branches on the probe result") {
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t fc = add(g, NodeType::FindColor, 300);
    node(g, fc).props["zoneIndex"] = Value::makeInt(-1);  // arbitrary region
    uint32_t cT = add(g, NodeType::Click, 600);
    node(g, cT).props["point"] = Value::makeCoord({0.3f, 0.3f});
    node(g, cT).props["holdMs"] = Value::makeInt(15);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", fc);
    link(g, fc, "found_exec", cT);
    link(g, fc, "not_found_exec", e);
    link(g, cT, "out", e);

    SUBCASE("found -> click through found_exec with centroid output") {
        FakeHost fh;
        fh.colorScan.found = true;
        fh.colorScan.center = {0.62f, 0.41f};
        fh.colorScan.similarity = 0.87f;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        CHECK(fh.touches.size() == 2);
        CHECK(r.nodeOutput(fc, "coordinates")->asCoord().x == doctest::Approx(0.62f));
        CHECK(r.nodeOutput(fc, "similarity")->asFloat() == doctest::Approx(0.87));
    }
    SUBCASE("miss -> not_found_exec") {
        FakeHost fh;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        CHECK(fh.touches.empty());
    }
}

TEST_CASE("engine: RunMacro delegates to the host and CheckColorZone branches") {
    Automation a;
    a.zones.push_back(Zone{});
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t rm = add(g, NodeType::RunMacro, 300);
    node(g, rm).props["macroIndex"] = Value::makeInt(2);
    uint32_t cz = add(g, NodeType::CheckColorZone, 600);
    node(g, cz).props["zoneIndex"] = Value::makeInt(0);
    uint32_t e = add(g, NodeType::End, 900);
    link(g, s, "out", rm);
    link(g, rm, "out", cz);
    link(g, cz, "present_exec", e);
    link(g, cz, "absent_exec", e);

    FakeHost fh;
    fh.zone = true;
    GraphRunner r(fh.host());
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0);
    CHECK(r.state() == RunState::Completed);
    CHECK(fh.ranMacro == 2);
    const Value* p = r.nodeOutput(cz, "present");
    REQUIRE(p != nullptr);
    CHECK(p->asBool());
}

// ---------------------------------------------------------------------------
// Phase 10 — large-graph performance measurements (real wall-clock numbers;
// these are sanity bounds, not lab benchmarks).
// ---------------------------------------------------------------------------
TEST_CASE("engine perf: 800-node linear graph builds, validates, serializes, runs") {
    using Clock = std::chrono::steady_clock;
    Automation a;
    AutomationGraph g;
    g.name = "BigLinear";

    const int N = 800;
    uint32_t s = add(g, NodeType::Start, 0);
    uint32_t prev = s;
    const char* prevPort = "out";

    const auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        uint32_t n = add(g, NodeType::CommentNode, 300.0f + i * 220.0f);
        node(g, n).props["text"] = Value::makeString("step " + std::to_string(i));
        link(g, prev, prevPort, n);
        prev = n;
    }
    uint32_t e = add(g, NodeType::End, 300.0f + N * 220.0f);
    link(g, prev, "out", e);
    const auto buildMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    const auto v0 = Clock::now();
    const auto issues = validateGraph(g);
    const auto valMs = std::chrono::duration<double, std::milli>(Clock::now() - v0).count();
    CHECK(issues.empty());

    const auto j0 = Clock::now();
    const std::string js = graphToJson(g);
    const auto serMs = std::chrono::duration<double, std::milli>(Clock::now() - j0).count();
    const auto p0 = Clock::now();
    AutomationGraph g2;
    std::string perr;
    REQUIRE(graphFromJson(js, g2, perr));
    const auto parMs = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
    CHECK(g2.nodes.size() == g.nodes.size());
    CHECK(g2.connections.size() == g.connections.size());

    const auto a0 = Clock::now();
    g2.autoArrange();
    const auto arrMs = std::chrono::duration<double, std::milli>(Clock::now() - a0).count();

    FakeHost fh;
    GraphRunner r(fh.host());
    const auto r0 = Clock::now();
    REQUIRE(r.start(a, g, 0));
    runUntilDone(r, 0, 1000);
    const auto runMs = std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
    CHECK(r.state() == RunState::Completed);

    MESSAGE("800-node graph: build=", buildMs, "ms validate=", valMs, "ms serialize=", serMs,
            "ms parse=", parMs, "ms arrange=", arrMs, "ms run=", runMs, "ms");
    // Generous bounds so slow CI still passes while real regressions fail loudly.
    CHECK(buildMs < 3000.0);
    CHECK(valMs < 3000.0);
    CHECK(serMs < 3000.0);
    CHECK(parMs < 3000.0);
    CHECK(arrMs < 3000.0);
    CHECK(runMs < 3000.0);
}

TEST_CASE("engine perf: 300-node If chain pulling data wires stays tick-cheap") {
    using Clock = std::chrono::steady_clock;
    Automation a;
    AutomationGraph g;
    uint32_t s = add(g, NodeType::Start, 0);
    GraphVariable gv0;
    gv0.name = "v";
    gv0.type = ValueType::Boolean;
    gv0.initialValue = Value::makeBool(true);
    g.upsertVariable(gv0);

    // 300 chained Ifs — each pulls its condition through a data wire from a
    // distinct Get Variable node (600 data wires total).
    uint32_t prev = s;
    const char* prevPort = "out";
    for (int i = 0; i < 300; ++i) {
        uint32_t gv = add(g, NodeType::GetVariable, 300.0f, 200.0f + i * 20.0f);
        node(g, gv).props["name"] = Value::makeString("v");
        uint32_t cond = add(g, NodeType::If, 600.0f, 200.0f + i * 20.0f);
        link(g, gv, "value", cond, "condition");
        link(g, prev, prevPort, cond);
        prev = cond;
        prevPort = "true";
    }
    uint32_t e = add(g, NodeType::End, 900);
    link(g, prev, "true", e);

    FakeHost fh;
    GraphRunner r(fh.host());
    const auto v0 = Clock::now();
    const auto issues = validateGraph(g);
    const auto valMs = std::chrono::duration<double, std::milli>(Clock::now() - v0).count();
    CHECK(issues.empty());

    REQUIRE(r.start(a, g, 0));
    const auto t0 = Clock::now();
    runUntilDone(r, 0, 1000);
    const auto runMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    CHECK(r.state() == RunState::Completed);
    MESSAGE("300-If chain: validate=", valMs, "ms run=", runMs, "ms");
    CHECK(valMs < 3000.0);
    CHECK(runMs < 3000.0);
}

TEST_CASE("engine: start(entryNodeId) runs only that Start's flow") {
    Automation a;
    AutomationGraph g;
    // Flow A: StartA -> End (no click); Flow B: StartB -> Click -> End.
    const uint32_t sa = add(g, NodeType::Start, 0);
    const uint32_t ea = add(g, NodeType::End, 300);
    link(g, sa, "out", ea);
    const uint32_t sb = add(g, NodeType::Start, 0);
    const uint32_t cb = add(g, NodeType::Click, 300);
    node(g, cb).props["point"] = Value::makeCoord({0.5f, 0.5f});
    const uint32_t eb = add(g, NodeType::End, 600);
    link(g, sb, "out", cb);
    link(g, cb, "out", eb);

    // Default entry = first Start -> flow A only (no touch).
    {
        FakeHost fh;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        CHECK(fh.touches.empty());
    }
    // Explicit entry = second Start -> flow B performs the click.
    {
        FakeHost fh;
        GraphRunner r(fh.host());
        REQUIRE(r.start(a, g, 0, sb));
        runUntilDone(r, 0);
        CHECK(r.state() == RunState::Completed);
        CHECK(fh.touches.size() == 2);
    }
}
