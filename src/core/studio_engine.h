// NEXPLAY Automation Studio — graph execution engine.
// Non-blocking, tick-based interpreter for studio::AutomationGraph. Pure core:
// no ImGui, no OS calls — the app feeds time and provides host sinks.
#pragma once
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/automation.h"

namespace wasd::studio {

// Per-node visual/debug state reported to the canvas (Phase 8 debug mode).
enum class NodeRunState : uint8_t { Idle, Active, Waiting, Done, Error, Skipped };

// What the app must provide for the graph to touch the real world. Mirrors
// MacroSinks plus a macro launcher and a log channel for the console panel.
struct GraphHost {
    std::function<void(int action /*0 down,1 up,2 move*/, int finger, Vec2 n)> touch;
    std::function<void(int vk, bool down)> key;
    std::function<std::optional<bool>(int zone)> zonePresent;    // nullopt: no frame
    std::function<std::optional<bool>(int image)> imagePresent;
    std::function<ImageScanResult(int image, const ImageQuery&)> imageScan;
    std::function<void(int macroIndex)> runMacro;
    std::function<void(int level /*0 info,1 warn,2 err*/, const std::string& msg)> log;
    // Async probes: nullopt means the readback is still in flight — the engine
    // re-queries every tick until a fresh result arrives.
    std::function<std::optional<ColorVal>(Vec2 normalizedPoint)> pixelColor;
    std::function<std::optional<ColorScanResult>(const Rect& region, const ColorVal& target)> colorScan;
    // Object detection (YOLO/ONNX). nullopt while readback+inference in flight.
    // `classFilter` holds model class NAMES (empty = all classes); the host
    // resolves them to ids — unknown names match nothing.
    std::function<std::optional<ObjectScanResult>(const Rect& region,
                                                 const std::vector<std::string>& classFilter,
                                                 float confidence)> objectScan;
};

// One entry of the execution console (debug timeline).
struct GraphEvent {
    int64_t us = 0;
    int level = 0;  // 0 info, 1 warn, 2 error
    uint32_t nodeId = 0;
    std::string text;
};

// Executes an AutomationGraph without blocking the caller. Semantics:
//  - Execution wires drive control flow; data wires are pull-evaluated lazily
//    with per-step memoization (impure producers reuse their stored outputs).
//  - Loop bodies may either wire back into the loop's exec-in pin or simply
//    dead-end; a dead end inside an iterating loop re-triggers the loop.
//  - All touches/keys the graph presses are released on finish/cancel/error.
//  - maxDurationMs / maxStepsBudget (graph metadata) bound every run.
class GraphRunner {
public:
    explicit GraphRunner(GraphHost host) : host_(std::move(host)) {}

    // `entryNodeId` selects which Start node to launch at (a graph may host
    // several independent flows); 0 = the first Start node.
    bool start(const Automation& a, const AutomationGraph& g, int64_t nowUs, uint32_t entryNodeId = 0);
    void tick(int64_t nowUs);
    void cancel(const char* reason = "cancelled");

    RunState state() const { return state_; }
    bool active() const { return state_ == RunState::Running || state_ == RunState::Waiting; }
    const std::string& message() const { return message_; }
    int executedNodes() const { return executed_; }
    uint64_t epoch() const { return epoch_; }
    uint32_t currentNodeId() const { return cur_.node; }

    // ---- debug / inspection (Phase 8) ----
    void setDebug(bool on) { debug_ = on; }
    bool paused() const { return paused_; }
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }
    void stepOnce();                                     // run exactly one node, then pause
    NodeRunState nodeState(uint32_t nodeId) const;
    const Value* nodeOutput(uint32_t nodeId, const std::string& portKey) const;
    std::string wireValue(uint32_t connId) const;
    const std::map<std::string, Value>& vars() const { return vars_; }
    const std::vector<GraphEvent>& events() const { return events_; }
    int64_t elapsedUs(int64_t nowUs) const { return nowUs - startUs_; }

private:
    struct Cursor {
        const AutomationGraph* g = nullptr;
        uint32_t node = 0;
    };
    struct SeqCont {  // Sequence node waiting for the current branch to end
        const AutomationGraph* g;
        uint32_t node;
        int nextBranch;
    };
    struct LoopFrame {  // an iterating loop; seqDepth scopes Break/Continue cleanup
        const AutomationGraph* g;
        uint32_t node;
        size_t seqDepth;
    };
    struct NodeRt {   // per-activation runtime: timers, phases, loop counters
        int64_t untilUs = 0;
        int64_t nextPollUs = 0;
        int64_t iter = 0;
        int64_t aux = 0;   // generic counter (image attempts, click phases)
        float auxF = 0.0f; // generic float (last scan score for diagnostics)
        int phase = 0;
        Vec2 from{}, to{};
    };

    // stepping
    void stepSlice(int64_t nowUs);
    void runNode(const AutomationGraph& g, const GraphNode& n, int64_t nowUs);
    uint32_t followExecOut(const AutomationGraph& g, uint32_t nodeId, const char* portKey) const;
    void advanceOut(const GraphNode& n, const char* portKey = "out");
    void advanceDisabled(const GraphNode& n);  // pass-through for a disabled node
    uint64_t rtKey(const AutomationGraph& g, uint32_t nodeId) const;
    bool popContinuation();  // dead-end: sequence branch -> loop back -> call return
    void finish(RunState s, const std::string& msg = {});
    void fail(const std::string& why);
    void releaseAll();

    // data evaluation (pull model)
    Value evalIn(const GraphNode& n, const char* key);
    Value evalOut(const AutomationGraph& g, uint32_t nodeId, const std::string& portKey);
    std::unordered_map<std::string, Value> computePure(const GraphNode& n);
    void setOut(const AutomationGraph& g, uint32_t nodeId, const char* key, Value v);

    // helpers
    int propInt(const GraphNode& n, const char* key, int def = 0) const;
    float propFloat(const GraphNode& n, const char* key, float def = 0.0f) const;
    bool propBool(const GraphNode& n, const char* key, bool def = false) const;
    std::string propStr(const GraphNode& n, const char* key, const char* def = "") const;
    Vec2 pointOf(const GraphNode& n, const char* key);
    int fingerOf(const GraphNode& n) const;
    void emitTouch(int action, int fi, Vec2 p);
    void emitKey(int vk, bool down);
    void emitLog(int level, const std::string& text, uint32_t nodeId = 0);
    void markState(uint32_t nodeId, NodeRunState s);
    const AutomationGraph& curGraph() const { return *cur_.g; }

    GraphHost host_;
    Automation automation_;
    AutomationGraph graph_;  // run uses a snapshot — live edits can't corrupt it
    Cursor cur_;
    std::vector<Cursor> callStack_;          // Function / Subgraph returns
    std::vector<SeqCont> seqStack_;          // pending Sequence branches
    std::vector<LoopFrame> loopStack_;       // iterating loops, innermost last
    // Node ids are unique per graph, not globally — key runtime maps by (graph, id)
    std::unordered_map<uint64_t, NodeRt> rt_;
    std::unordered_map<uint64_t, std::unordered_map<std::string, Value>> outputs_;
    std::unordered_map<uint64_t, std::unordered_map<std::string, Value>> memo_;  // per-step
    std::unordered_map<uint32_t, std::string> wireValues_;
    std::unordered_map<uint32_t, NodeRunState> nodeStates_;
    std::map<std::string, Value> vars_;
    std::vector<GraphEvent> events_;
    std::set<uint32_t> bpHit_;               // breakpoints already honored this run

    RunState state_ = RunState::Idle;
    uint64_t epoch_ = 0;
    int executed_ = 0;
    int subDepth_ = 0;
    int64_t startUs_ = 0;
    int64_t lastTickUs_ = 0;
    int64_t maxDurationUs_ = 60000000;
    int maxStepsBudget_ = 100000;
    bool debug_ = false, paused_ = false, stepOnce_ = false;
    bool fingerDown_[kMaxFingers] = {};
    std::set<int> keysDown_;
    std::mt19937 rng_{std::random_device{}()};
    std::string message_;
};

}  // namespace wasd::studio
