#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/profile.h"
#include "core/studio_graph.h"

using namespace wasd;
using namespace wasd::studio;

TEST_CASE("Studio catalog covers all 7 categories and FindImage exposes full data & branch pins") {
    const auto& specs = allNodeSpecs();
    CHECK(specs.size() >= 54);

    bool seenCat[7] = {};
    for (const auto& s : specs) seenCat[static_cast<int>(s.category)] = true;
    for (int i = 0; i < 7; ++i) CHECK(seenCat[i]);

    AutomationGraph g = makeStarterGraph("Test");
    GraphNode& fi = g.addNode(NodeType::FindImage, {300.0f, 180.0f});
    REQUIRE(fi.findInputByKey("in") != nullptr);
    REQUIRE(fi.findOutputByKey("found_exec") != nullptr);
    REQUIRE(fi.findOutputByKey("not_found_exec") != nullptr);
    REQUIRE(fi.findOutputByKey("found") != nullptr);
    REQUIRE(fi.findOutputByKey("similarity") != nullptr);
    REQUIRE(fi.findOutputByKey("coordinates") != nullptr);
    REQUIRE(fi.findOutputByKey("duration") != nullptr);
    REQUIRE(fi.findOutputByKey("out_region") != nullptr);
    REQUIRE(fi.findOutputByKey("result") != nullptr);
    CHECK(fi.findOutputByKey("found")->dataType == ValueType::Boolean);
    CHECK(fi.findOutputByKey("similarity")->dataType == ValueType::Float);
    CHECK(fi.findOutputByKey("coordinates")->dataType == ValueType::Coordinate);
    CHECK(fi.findOutputByKey("result")->dataType == ValueType::ImageResult);
}

TEST_CASE("Studio node creation, deletion, group cleanup, and copy/paste preserve internal wires") {
    AutomationGraph g;
    g.id = generateGraphId();
    g.name = "Edit Ops";

    const uint32_t stId = g.addNode(NodeType::Start, {50.0f, 100.0f}).id;
    const uint32_t fiId = g.addNode(NodeType::FindImage, {300.0f, 100.0f}).id;
    const uint32_t clId = g.addNode(NodeType::Click, {620.0f, 100.0f}).id;

    REQUIRE(g.connectByKey(stId, "out", fiId, "in").ok);
    REQUIRE(g.connectByKey(fiId, "found_exec", clId, "in").ok);
    REQUIRE(g.connectByKey(fiId, "coordinates", clId, "point").ok);
    CHECK(g.connections.size() == 3);

    g.addGroup("Detection Stage", {260.0f, 60.0f, 620.0f, 220.0f}, {fiId, clId});
    REQUIRE(g.groups.front().nodeIds.size() == 2);

    // Copy FindImage + Click together: internal exec + data wires must be cloned with fresh IDs.
    const GraphClipboard clip = g.copySelection({fiId, clId});
    CHECK(clip.nodes.size() == 2);
    CHECK(clip.internalConnections.size() == 2);  // Start->FindImage excluded because Start wasn't selected

    const auto pasted = g.pasteClipboard(clip, {40.0f, 220.0f});
    REQUIRE(pasted.size() == 2);
    CHECK(g.nodes.size() == 5);
    CHECK(g.connections.size() == 5);
    CHECK(validateGraph(g).empty());

    // Deleting original FindImage removes its 3 incident wires and cleans up group membership.
    REQUIRE(g.removeNode(fiId));
    CHECK(g.nodes.size() == 4);
    CHECK(g.connections.size() == 2);  // only pasted pair wires remain
    CHECK(g.groups.front().nodeIds.size() == 1);
    CHECK(g.groups.front().nodeIds.front() == clId);
    CHECK(validateGraph(g).empty());
}

TEST_CASE("Port compatibility enforces Exec vs Data separation, type rules, reverse drag, and data DAG") {
    AutomationGraph g = makeStarterGraph("Port Rules");
    const uint32_t startId = g.nodes.front().id;
    const uint32_t fiId = g.addNode(NodeType::FindImage, {300.0f, 120.0f}).id;
    const uint32_t clId = g.addNode(NodeType::Click, {600.0f, 120.0f}).id;
    const uint32_t fcId = g.addNode(NodeType::FindColor, {300.0f, 360.0f}).id;
    const uint32_t a1Id = g.addNode(NodeType::MathAdd, {300.0f, 520.0f}).id;
    const uint32_t a2Id = g.addNode(NodeType::MathMultiply, {560.0f, 520.0f}).id;

    // 1. Execution <-> Data is strictly forbidden with an explicit reason.
    const auto execToData = g.canConnect(startId, g.findNode(startId)->findOutputByKey("out")->id,
                                         clId, g.findNode(clId)->findInputByKey("point")->id);
    CHECK_FALSE(execToData.ok);
    CHECK(execToData.code == RejectReason::KindMismatch);
    CHECK(execToData.reason.find("Execution") != std::string::npos);

    // 2. Incompatible Data types (Color -> Coordinate) rejected with type names in reason.
    const auto badType = g.canConnect(fcId, g.findNode(fcId)->findOutputByKey("color")->id,
                                      clId, g.findNode(clId)->findInputByKey("point")->id);
    CHECK_FALSE(badType.ok);
    CHECK(badType.code == RejectReason::TypeMismatch);
    CHECK(badType.reason.find("Color") != std::string::npos);
    CHECK(badType.reason.find("Coordinate") != std::string::npos);

    // 3. Self-connection rejected.
    const auto selfConn = g.canConnect(clId, g.findNode(clId)->findOutputByKey("out")->id,
                                       clId, g.findNode(clId)->findInputByKey("in")->id);
    CHECK_FALSE(selfConn.ok);
    CHECK(selfConn.code == RejectReason::SelfConnection);

    // 4. Output -> Output rejected.
    const auto sameDir = g.canConnect(startId, g.findNode(startId)->findOutputByKey("out")->id,
                                      clId, g.findNode(clId)->findOutputByKey("out")->id);
    CHECK_FALSE(sameDir.ok);
    CHECK(sameDir.code == RejectReason::SameDirection);

    // 5. Dragging backwards (Input -> Output) normalizes cleanly to Output -> Input.
    const auto rev = g.connect(clId, g.findNode(clId)->findInputByKey("point")->id,
                               fiId, g.findNode(fiId)->findOutputByKey("coordinates")->id);
    REQUIRE(rev.ok);
    CHECK(rev.outNode == fiId);
    CHECK(rev.inNode == clId);

    // 6. Execution back-edge (retry loop Click.out -> FindImage.in) IS allowed.
    REQUIRE(g.connect(startId, g.findNode(startId)->findOutputByKey("out")->id,
                      fiId, g.findNode(fiId)->findInputByKey("in")->id).ok);
    REQUIRE(g.connect(fiId, g.findNode(fiId)->findOutputByKey("found_exec")->id,
                      clId, g.findNode(clId)->findInputByKey("in")->id).ok);
    REQUIRE(g.connect(clId, g.findNode(clId)->findOutputByKey("out")->id,
                      fiId, g.findNode(fiId)->findInputByKey("in")->id).ok);

    // 7. Data cycle (Add.result -> Mul.a AND Mul.result -> Add.b) is rejected!
    REQUIRE(g.connect(a1Id, g.findNode(a1Id)->findOutputByKey("result")->id,
                      a2Id, g.findNode(a2Id)->findInputByKey("a")->id).ok);
    const auto dataCycle = g.canConnect(a2Id, g.findNode(a2Id)->findOutputByKey("result")->id,
                                        a1Id, g.findNode(a1Id)->findInputByKey("b")->id);
    CHECK_FALSE(dataCycle.ok);
    CHECK(dataCycle.code == RejectReason::DataCycle);
}

TEST_CASE("Graph serialization, atomic file save/backup, and v0->v1 migration preserve visual & functional state") {
    AutomationGraph g = makeTemplateGraph(GraphTemplateKind::ImageRecognitionExample);
    g.view.pan = {-145.5f, 82.25f};
    g.view.zoom = 1.35f;
    g.view.wireStyle = WireStyle::Rounded;
    g.view.showMinimap = false;
    g.metadata.author = "NEXPLAY QA";
    g.metadata.triggerKey = 0x74;  // F5
    g.metadata.maxStepsBudget = 25000;
    g.upsertVariable({"attempts", ValueType::Integer, Value::makeInt(3), "Max tries"});
    g.upsertVariable({"lootBox", ValueType::Array,
                      Value::makeArray({Value::makeString("medkit"), Value::makeInt(2)}), ""});
    auto& grp = g.addGroup("Vision Stage", {250.0f, 100.0f, 450.0f, 280.0f}, {g.nodes[1].id});
    grp.collapsed = true;
    g.addNote("Tip", "Adjust threshold if lighting changes", {80.0f, 380.0f});
    g.nodes[1].breakpoint = true;
    g.nodes[1].title = "Detect Start Button";

    AutomationGraph sub = makeTemplateGraph(GraphTemplateKind::BasicSequence);
    sub.name = "ConfirmHelper";
    g.subgraphs.push_back(sub);

    auto dir = std::filesystem::temp_directory_path() / "nexplay_studio_graph_tests";
    std::filesystem::remove_all(dir);
    const std::string path = (dir / "hunter.nexgraph.json").string();
    std::string err;
    REQUIRE_MESSAGE(saveGraphFile(path, g, err), err);

    // Second save creates .bak backup
    g.name = "Updated Hunter Flow";
    REQUIRE_MESSAGE(saveGraphFile(path, g, err), err);
    CHECK(std::filesystem::exists(path + ".bak"));

    AutomationGraph loaded;
    REQUIRE_MESSAGE(loadGraphFile(path, loaded, err), err);
    CHECK(loaded == g);
    CHECK(loaded.view.pan.x == doctest::Approx(-145.5f));
    CHECK(loaded.view.zoom == doctest::Approx(1.35f));
    CHECK(loaded.view.wireStyle == WireStyle::Rounded);
    CHECK_FALSE(loaded.view.showMinimap);
    CHECK(loaded.groups.front().collapsed);
    CHECK(loaded.nodes[1].breakpoint);
    CHECK(loaded.nodes[1].title == "Detect Start Button");
    CHECK(loaded.subgraphs.size() == 1);
    CHECK(loaded.findVariable("attempts") != nullptr);
    CHECK(loaded.findVariable("lootBox")->initialValue.arr.size() == 2);

    std::filesystem::remove_all(dir);

    // Version 0 migration check ("links" -> "connections", missing "view")
    const char* v0Json = R"({
        "id": "legacy_0",
        "name": "Draft v0",
        "nodes": [
            {"id": 1, "type": "flow.start", "pos": [10, 20],
             "inputs": [], "outputs": [{"id": 2, "key": "out", "name": "Exec", "kind": "exec", "dataType": "Boolean", "dir": "out"}]},
            {"id": 3, "type": "flow.end", "pos": [220, 20],
             "inputs": [{"id": 4, "key": "in", "name": "Exec", "kind": "exec", "dataType": "Boolean", "dir": "in"}], "outputs": []}
        ],
        "links": [{"id": 5, "fromNode": 1, "fromPort": 2, "toNode": 3, "toPort": 4, "kind": "exec"}]
    })";
    AutomationGraph migratedGraph;
    std::vector<std::string> notes;
    REQUIRE_MESSAGE(graphFromJson(v0Json, migratedGraph, err, &notes), err);
    CHECK(migratedGraph.version == kGraphSchemaVersion);
    CHECK(migratedGraph.connections.size() == 1);
    CHECK_FALSE(notes.empty());

    // Undo / Redo history check
    GraphHistory hist;
    hist.reset(loaded);
    loaded.addNode(NodeType::Delay, {500.0f, 500.0f});
    CHECK(hist.commit(loaded));
    CHECK(hist.canUndo());
    AutomationGraph undone;
    REQUIRE(hist.undo(undone));
    CHECK(undone == g);
    AutomationGraph redone;
    REQUIRE(hist.redo(redone));
    CHECK(redone.nodes.size() == g.nodes.size() + 1);

    // Profile integration: studioGraphs round-trip inside Profile without disturbing legacy fields
    Profile prof = makeDefaultProfile();
    prof.automation.studioGraphs.push_back(g);
    Profile profOut;
    REQUIRE_MESSAGE(profileFromJson(profileToJson(prof), profOut, err), err);
    REQUIRE(profOut.automation.studioGraphs.size() == 1);
    CHECK(profOut.automation.studioGraphs.front() == g);
}

TEST_CASE("Custom plugin node registration works without modifying core node enum") {
    NodeTypeSpec custom;
    custom.typeKey = "plugin.log_banner";
    custom.displayName = "Log Banner";
    custom.category = NodeCategory::NexplayCommands;
    custom.icon = "\uE946";
    custom.summary = "Custom registered plugin node";
    custom.ports = {{"in", "In", PortKind::Execution, ValueType::Boolean, PortDir::Input, {}},
                    {"out", "Out", PortKind::Execution, ValueType::Boolean, PortDir::Output, {}},
                    {"msg", "Message", PortKind::Data, ValueType::String, PortDir::Input, Value::makeString("Hello")}};
    custom.properties = {{"prefix", "Prefix", Value::makeString("[NEX]"), false, 0, 0, ""}};
    registerCustomNodeSpec(custom);

    AutomationGraph g = makeStarterGraph("Plugin Graph");
    GraphNode& pn = g.addNode(NodeType::CustomPlugin, {320.0f, 180.0f}, "plugin.log_banner");
    REQUIRE(pn.findInputByKey("msg") != nullptr);
    CHECK(pn.getProp("prefix").asString() == "[NEX]");

    std::string err;
    AutomationGraph out;
    REQUIRE_MESSAGE(graphFromJson(graphToJson(g), out, err), err);
    CHECK(out.nodes.back().customTypeKey == "plugin.log_banner");
    CHECK(out.nodes.back().getProp("prefix").asString() == "[NEX]");
}

TEST_CASE("Studio per-Start trigger keys serialize and duplicate keys are flagged") {
    AutomationGraph g = makeStarterGraph("Triggers");
    const uint32_t s2 = g.addNode(NodeType::Start, {400.0f, 100.0f}).id;
    g.findNode(s2)->triggerKey = 0x47;   // G
    g.findNode(s2)->triggerKey2 = 0x52;  // R
    g.metadata.triggerKey = 0x44;        // D (legacy fallback for first Start)

    const std::string js = graphToJson(g);
    AutomationGraph back;
    std::string err;
    REQUIRE_MESSAGE(graphFromJson(js, back, err), err);
    const GraphNode* n = back.findNode(s2);
    REQUIRE(n != nullptr);
    CHECK(n->triggerKey == 0x47);
    CHECK(n->triggerKey2 == 0x52);
    CHECK(back.metadata.triggerKey == 0x44);

    // Same hotkey on two Starts is a validation warning, not a silent trap.
    g.findNode(s2)->triggerKey = g.metadata.triggerKey = 0x47;
    g.findNode(s2)->triggerKey2 = 0;
    bool dup = false;
    for (const auto& s : validateGraph(g)) dup = dup || s.find("duplicate trigger key") != std::string::npos;
    CHECK(dup);
}
