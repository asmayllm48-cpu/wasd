#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/studio_converter.h"
#include "core/studio_engine.h"

using namespace wasd;
using namespace wasd::studio;

namespace {

const GraphNode* findType(const AutomationGraph& g, NodeType t, int nth = 0) {
    for (const auto& n : g.nodes)
        if (n.type == t && nth-- == 0) return &n;
    return nullptr;
}
int countType(const AutomationGraph& g, NodeType t) {
    int c = 0;
    for (const auto& n : g.nodes)
        if (n.type == t) ++c;
    return c;
}

}  // namespace

TEST_CASE("converter: linear macro steps map to equivalent nodes and stay wired") {
    Automation a;
    Macro m;
    m.name = "Linear";
    Step s1;
    s1.type = StepType::Tap;
    s1.a = {0.4f, 0.6f};
    m.steps.push_back(s1);
    Step s2;
    s2.type = StepType::Wait;
    s2.ms = 120;
    m.steps.push_back(s2);
    Step s3;
    s3.type = StepType::Key;
    s3.key = 0x41;
    s3.keyMode = KeyMode::Down;
    m.steps.push_back(s3);

    ConversionReport rep = macroToGraph(m, a);
    CHECK(rep.stepsConverted == 3);
    REQUIRE(findType(rep.graph, NodeType::Start));
    REQUIRE(findType(rep.graph, NodeType::Click));
    REQUIRE(findType(rep.graph, NodeType::Delay));
    REQUIRE(findType(rep.graph, NodeType::KeyPress));
    REQUIRE(findType(rep.graph, NodeType::End));
    CHECK(rep.graph.metadata.sourceLegacyMacro == "Linear");
    // steps are untouched — non-destructive contract
    CHECK(m.steps.size() == 3);
    CHECK(m.steps[0].type == StepType::Tap);
    CHECK(validateGraph(rep.graph).empty());
}

TEST_CASE("converter: If/Loop/ImageRecognize map to branching exec flows") {
    Automation a;
    a.zones.push_back(Zone{});
    a.images.push_back(ImageRef{});
    Macro m;
    m.name = "Branchy";

    Step iff;
    iff.type = StepType::If;
    iff.zone = 0;
    {
        Step t;
        t.type = StepType::Tap;
        iff.then.push_back(t);
        Step o;
        o.type = StepType::Comment;
        o.text = "else path";
        iff.otherwise.push_back(o);
    }
    m.steps.push_back(iff);

    Step lp;
    lp.type = StepType::Loop;
    lp.loopMode = LoopMode::Count;
    lp.count = 4;
    {
        Step b;
        b.type = StepType::Swipe;
        lp.then.push_back(b);
    }
    m.steps.push_back(lp);

    Step ir;
    ir.type = StepType::ImageRecognize;
    ir.image = 0;
    ir.ms = 800;
    ir.foundAction = ImageFoundAction::ClickMatch;
    ir.notFoundAction = ImageNotFoundAction::RunSteps;
    {
        Step w;
        w.type = StepType::Wait;
        w.ms = 200;
        ir.otherwise.push_back(w);
    }
    m.steps.push_back(ir);

    ConversionReport rep = macroToGraph(m, a);
    CHECK(validateGraph(rep.graph).empty());
    REQUIRE(findType(rep.graph, NodeType::CheckColorZone));
    REQUIRE(findType(rep.graph, NodeType::Loop));
    REQUIRE(findType(rep.graph, NodeType::FindImage));
    REQUIRE(findType(rep.graph, NodeType::GetImageCoordinates));  // ClickMatch offset path
    REQUIRE(findType(rep.graph, NodeType::Click));
    REQUIRE(countType(rep.graph, NodeType::CommentNode) >= 1);
}

TEST_CASE("converter: While loop restructures into bounded loop + condition + break") {
    Automation a;
    a.zones.push_back(Zone{});
    Macro m;
    m.name = "WhileLoop";
    Step lp;
    lp.type = StepType::Loop;
    lp.loopMode = LoopMode::While;
    lp.zone = 0;
    lp.count = 50;
    {
        Step t;
        t.type = StepType::Tap;
        lp.then.push_back(t);
    }
    m.steps.push_back(lp);

    ConversionReport rep = macroToGraph(m, a);
    CHECK(validateGraph(rep.graph).empty());
    REQUIRE(findType(rep.graph, NodeType::Loop));
    REQUIRE(findType(rep.graph, NodeType::CheckColorZone));
    REQUIRE(findType(rep.graph, NodeType::Break));
}

TEST_CASE("converter: converted macro round-trips back to equivalent steps") {
    Automation a;
    a.zones.push_back(Zone{});
    Macro m;
    m.name = "RoundTrip";
    Step s1;
    s1.type = StepType::Tap;
    s1.a = {0.3f, 0.7f};
    s1.ms = 50;
    m.steps.push_back(s1);
    Step s2;
    s2.type = StepType::Wait;
    s2.ms = 250;
    m.steps.push_back(s2);
    Step s3;
    s3.type = StepType::Stop;
    m.steps.push_back(s3);

    ConversionReport crep = macroToGraph(m, a);
    SimplifyReport srep = graphToMacroSteps(crep.graph);
    CHECK(srep.warnings.empty());
    REQUIRE(srep.steps.size() == 3);
    CHECK(srep.steps[0].type == StepType::Tap);
    CHECK(srep.steps[0].a.x == doctest::Approx(0.3f));
    CHECK(srep.steps[1].type == StepType::Wait);
    CHECK(srep.steps[1].ms == 250);
    CHECK(srep.steps[2].type == StepType::Stop);
}

TEST_CASE("converter: graph-to-steps reports lossy nodes instead of dropping silently") {
    AutomationGraph g;
    uint32_t s = g.addNode(NodeType::Start, {0, 0}).id;
    uint32_t sv = g.addNode(NodeType::SetVariable, {300, 0}).id;
    g.findNode(sv)->props["name"] = Value::makeString("x");
    uint32_t e = g.addNode(NodeType::End, {600, 0}).id;
    g.connectByKey(s, "out", sv, "in");
    g.connectByKey(sv, "out", e, "in");

    SimplifyReport rep = graphToMacroSteps(g);
    CHECK(!rep.warnings.empty());          // SetVariable warned
    CHECK(rep.nodesDropped >= 1);
    CHECK(!rep.lossless());
}

TEST_CASE("converter: converted macro executes correctly in the engine") {
    Automation a;
    a.zones.push_back(Zone{});
    Macro m;
    m.name = "ExecMacro";
    Step s1;
    s1.type = StepType::Wait;
    s1.ms = 10;
    m.steps.push_back(s1);
    Step s2;
    s2.type = StepType::Loop;
    s2.loopMode = LoopMode::Count;
    s2.count = 2;
    {
        Step t;
        t.type = StepType::Tap;
        t.a = {0.5f, 0.5f};
        s2.then.push_back(t);
    }
    m.steps.push_back(s2);

    ConversionReport rep = macroToGraph(m, a);
    CHECK(validateGraph(rep.graph).empty());

    struct Sinks {
        std::vector<int> actions;
        GraphHost host() {
            GraphHost h;
            h.touch = [this](int a, int, Vec2) { actions.push_back(a); };
            return h;
        }
    } sinks;
    GraphRunner r(sinks.host());
    REQUIRE(r.start(a, rep.graph, 0));
    for (int64_t us = 0; r.active() && us < 5000000; us += 5000) r.tick(us);
    CHECK(r.state() == RunState::Completed);
    CHECK(sinks.actions.size() == 4);  // 2 taps * (down+up)
}
