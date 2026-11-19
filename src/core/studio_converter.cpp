// NEXPLAY Automation Studio — legacy macro <-> visual graph converter.
#include "core/studio_converter.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wasd::studio {
namespace {

// ---------------------------------------------------------------------------
// Macro -> Graph builder
// ---------------------------------------------------------------------------

struct Builder {
    ConversionReport& rep;
    const Automation& a;
    AutomationGraph& g;

    // Exec ports still waiting for their successor node. Branch ends merge here,
    // which is exactly the "join" semantic of the legacy step lists.
    std::vector<std::pair<uint32_t, std::string>> pending;

    struct DeferredJump {
        uint32_t nodeId;
        std::string portKey;
        int targetStep;
    };

    Builder(ConversionReport& r, const Automation& aut, AutomationGraph& graph)
        : rep(r), a(aut), g(graph) {}

    uint32_t add(NodeType t) {
        GraphNode& n = g.addNode(t, {0.0f, 0.0f});  // autoArrange() lays out at the end
        rep.nodesCreated++;
        return n.id;
    }
    void prop(uint32_t id, const char* key, Value v) {
        if (auto* n = g.findNode(id)) n->props[key] = std::move(v);
    }
    void wirePendingTo(uint32_t nodeId, const char* inKey = "in") {
        for (auto& [src, port] : pending) {
            auto chk = g.connectByKey(src, port, nodeId, inKey);
            if (!chk.ok)
                rep.notes.push_back("wire skipped: " + chk.reason);
        }
        pending.clear();
    }

    // Condition node for legacy zone/image checks. `image>=0` uses a Find Image
    // single check, otherwise a Color Zone presence test.
    uint32_t addCondition(const Step& s) {
        if (s.image >= 0) {
            uint32_t n = add(NodeType::FindImage);
            prop(n, "imageIndex", Value::makeInt(s.image));
            prop(n, "timeoutMs", Value::makeInt(0));  // single immediate check
            return n;
        }
        uint32_t n = add(NodeType::CheckColorZone);
        prop(n, "zoneIndex", Value::makeInt(std::max(0, s.zone)));
        return n;
    }
    // Exec-out keys of a condition node: {true-side, false-side}.
    static std::pair<const char*, const char*> condPorts(const Step& s) {
        return s.image >= 0 ? std::pair{"found_exec", "not_found_exec"}
                            : std::pair{"present_exec", "absent_exec"};
    }

    void chain(const std::vector<Step>& steps) {
        std::unordered_map<int, uint32_t> stepEntry;  // list-local JumpStep targets
        std::vector<DeferredJump> deferred;

        for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
            const Step& s = steps[i];
            rep.stepsConverted++;
            stepEntry[i] = emitStep(s, i, deferred);
        }
        for (const DeferredJump& dj : deferred) {
            auto it = stepEntry.find(dj.targetStep);
            if (it != stepEntry.end() && it->second) {
                g.connectByKey(dj.nodeId, dj.portKey, it->second, "in");
            } else {
                rep.notes.push_back("JumpStep target out of range — treated as continue");
            }
        }
    }

    // Emits the node(s) for one legacy step and returns the first node's id.
    // `pending` afterwards holds this step's open exec-outs.
    uint32_t emitStep(const Step& s, int idx, std::vector<DeferredJump>& deferred) {
        auto simple = [&](NodeType t) -> uint32_t {
            uint32_t n = add(t);
            wirePendingTo(n);
            pending = {{n, "out"}};
            if (auto* gn = g.findNode(n)) gn->enabled = s.enabled;
            return n;
        };
        auto note = [&](std::string t) {
            rep.notes.push_back("step " + std::to_string(idx + 1) + ": " + std::move(t));
            rep.warnings++;
        };
        switch (s.type) {
            case StepType::Wait: {
                uint32_t n = simple(NodeType::Delay);
                prop(n, "duration", Value::makeInt(s.ms));
                return n;
            }
            case StepType::Tap: {
                uint32_t n = simple(NodeType::Click);
                prop(n, "point", Value::makeCoord(s.a));
                prop(n, "holdMs", Value::makeInt(std::max(15, s.ms)));
                prop(n, "finger", Value::makeInt(s.finger));
                return n;
            }
            case StepType::LongPress: {
                uint32_t n = simple(NodeType::LongPress);
                prop(n, "point", Value::makeCoord(s.a));
                prop(n, "duration", Value::makeInt(std::max(40, s.ms)));
                prop(n, "finger", Value::makeInt(s.finger));
                return n;
            }
            case StepType::PressDown: {
                uint32_t n = simple(NodeType::PressDown);
                prop(n, "point", Value::makeCoord(s.a));
                prop(n, "finger", Value::makeInt(s.finger));
                return n;
            }
            case StepType::Release: {
                uint32_t n = simple(NodeType::Release);
                prop(n, "finger", Value::makeInt(s.finger));
                return n;
            }
            case StepType::Swipe: {
                uint32_t n = simple(NodeType::Slide);
                prop(n, "from", Value::makeCoord(s.a));
                prop(n, "to", Value::makeCoord(s.b));
                prop(n, "duration", Value::makeInt(std::max(16, s.ms)));
                prop(n, "finger", Value::makeInt(s.finger));
                return n;
            }
            case StepType::Key: {
                if (s.keyMode == KeyMode::Up) {
                    uint32_t n = simple(NodeType::KeyRelease);
                    prop(n, "key", Value::makeInt(s.key));
                    return n;
                }
                uint32_t n = simple(NodeType::KeyPress);
                prop(n, "key", Value::makeInt(s.key));
                if (s.keyMode == KeyMode::Down) prop(n, "holdOnly", Value::makeBool(true));
                else prop(n, "holdMs", Value::makeInt(std::max(15, s.ms)));
                return n;
            }
            case StepType::RunMacro: {
                uint32_t n = simple(NodeType::RunMacro);
                prop(n, "macroIndex", Value::makeInt(s.macro));
                return n;
            }
            case StepType::Comment: {
                uint32_t n = simple(NodeType::CommentNode);
                prop(n, "text", Value::makeString(s.text));
                return n;
            }
            case StepType::Break: {
                uint32_t n = add(NodeType::Break);
                wirePendingTo(n);
                pending.clear();  // Break never continues the line
                return n;
            }
            case StepType::Stop: {
                uint32_t n = add(NodeType::StopExecution);
                wirePendingTo(n);
                pending.clear();
                return n;
            }
            case StepType::If: {
                uint32_t chk = addCondition(s);
                wirePendingTo(chk);
                auto [tport, fport] = condPorts(s);
                if (s.negate) std::swap(tport, fport);
                pending = {{chk, tport}};
                chain(s.then);
                auto thenEnds = std::move(pending);
                pending = {{chk, fport}};
                chain(s.otherwise);
                pending.insert(pending.end(), thenEnds.begin(), thenEnds.end());
                if (s.then.empty()) pending.push_back({chk, tport});
                if (s.otherwise.empty()) pending.push_back({chk, fport});
                return chk;
            }
            case StepType::Loop: {
                uint32_t lp = add(NodeType::Loop);
                wirePendingTo(lp);
                if (s.loopMode == LoopMode::Count) {
                    prop(lp, "count", Value::makeInt(std::max(1, s.count)));
                    pending = {{lp, "body"}};
                    chain(s.then);
                    pending = {{lp, "completed"}};  // body dead-ends -> implicit back-edge
                } else {
                    // While/Until: bounded loop whose body starts by re-testing the
                    // condition; a failing test Breaks out through `completed`.
                    prop(lp, "count", Value::makeInt(std::max(1, s.count)));
                    uint32_t chk = addCondition(s);
                    g.connectByKey(lp, "body", chk, "in");
                    auto [cport, fport] = condPorts(s);
                    const bool until = s.loopMode == LoopMode::Until;
                    if (s.negate != until) std::swap(cport, fport);
                    uint32_t br = add(NodeType::Break);
                    g.connectByKey(chk, fport, br, "in");
                    pending = {{chk, cport}};
                    chain(s.then);
                    pending = {{lp, "completed"}};
                }
                return lp;
            }
            case StepType::ImageRecognize: {
                uint32_t fi = add(NodeType::FindImage);
                wirePendingTo(fi);
                prop(fi, "imageIndex", Value::makeInt(s.image));
                prop(fi, "timeoutMs", Value::makeInt(s.ms));
                prop(fi, "thresholdPct", Value::makeInt(s.thresholdPct));
                prop(fi, "regionMode", Value::makeInt(static_cast<int>(s.regionMode)));
                prop(fi, "regionX", Value::makeFloat(s.region.x));
                prop(fi, "regionY", Value::makeFloat(s.region.y));
                prop(fi, "regionW", Value::makeFloat(s.region.w));
                prop(fi, "regionH", Value::makeFloat(s.region.h));
                prop(fi, "intervalMs", Value::makeInt(s.intervalMs));
                prop(fi, "maxAttempts", Value::makeInt(s.retryCount > 0 ? s.retryCount : 1));
                prop(fi, "matchPolicy", Value::makeInt(static_cast<int>(s.matchPolicy)));
                prop(fi, "continuous", Value::makeBool(s.continuous));
                emitImageBranch(fi, "found_exec", s, true, deferred);
                emitImageBranch(fi, "not_found_exec", s, false, deferred);
                return fi;
            }
            case StepType::ImageSearch:
            case StepType::ClickImage:
                note("legacy image step had no direct conversion (already migrated by loader)");
                return 0;
        }
        note("unsupported step type — skipped");
        return 0;
    }

    // Builds the found/not-found action chain of an ImageRecognize step.
    void emitImageBranch(uint32_t fi, const char* port, const Step& s, bool found,
                         std::vector<DeferredJump>& deferred) {
        pending = {{fi, port}};
        if (found) {
            switch (s.foundAction) {
                case ImageFoundAction::Continue: break;  // port stays pending
                case ImageFoundAction::ClickMatch: {
                    if (s.delayMs > 0) {
                        uint32_t d = add(NodeType::Delay);
                        wirePendingTo(d);
                        prop(d, "duration", Value::makeInt(s.delayMs));
                        pending = {{d, "out"}};
                    }
                    uint32_t n = add(s.clickKind == ClickKind::LongPress ? NodeType::LongPress
                                     : s.clickKind == ClickKind::DoubleTap ? NodeType::DoubleClick
                                                                         : NodeType::Click);
                    wirePendingTo(n);
                    if (s.clickKind == ClickKind::LongPress)
                        prop(n, "duration", Value::makeInt(std::max(40, s.pressMs)));
                    // Found coordinates flow through a Get Image Coordinates node
                    // so the legacy clickOffset survives.
                    uint32_t gc = add(NodeType::GetImageCoordinates);
                    prop(gc, "offsetX", Value::makeFloat(s.clickOfs.x));
                    prop(gc, "offsetY", Value::makeFloat(s.clickOfs.y));
                    g.connectByKey(fi, "result", gc, "result");
                    g.connectByKey(gc, "coordinates", n, "point");
                    pending = {{n, "out"}};
                    break;
                }
                case ImageFoundAction::ClickPoint: {
                    if (s.delayMs > 0) {
                        uint32_t d = add(NodeType::Delay);
                        wirePendingTo(d);
                        prop(d, "duration", Value::makeInt(s.delayMs));
                        pending = {{d, "out"}};
                    }
                    uint32_t n = add(NodeType::Click);
                    wirePendingTo(n);
                    prop(n, "point", Value::makeCoord(s.foundClick));
                    pending = {{n, "out"}};
                    break;
                }
                case ImageFoundAction::RunMacro: {
                    uint32_t n = add(NodeType::RunMacro);
                    wirePendingTo(n);
                    prop(n, "macroIndex", Value::makeInt(s.foundMacro));
                    pending = {{n, "out"}};
                    break;
                }
                case ImageFoundAction::RunSteps: chain(s.then); break;
                case ImageFoundAction::JumpStep:
                    deferred.push_back({fi, port, s.jumpTo});
                    pending.clear();
                    break;
                case ImageFoundAction::Stop: {
                    uint32_t n = add(NodeType::StopExecution);
                    wirePendingTo(n);
                    pending.clear();
                    break;
                }
            }
            return;
        }
        switch (s.notFoundAction) {
            case ImageNotFoundAction::Continue: break;
            case ImageNotFoundAction::RetrySearch: {
                // Self-connections are forbidden — route the retry through a
                // Jump Target anchor back into the Find Image exec-in.
                uint32_t jt = add(NodeType::JumpTarget);
                prop(jt, "label", Value::makeString("retry_search"));
                g.connectByKey(fi, port, jt, "in");
                g.connectByKey(jt, "out", fi, "in");
                rep.notes.push_back("ON NOT FOUND retry became a re-entry anchor (bounded by Timeout)");
                pending.clear();
                break;
            }
            case ImageNotFoundAction::RunSteps: chain(s.otherwise); break;
            case ImageNotFoundAction::JumpStep:
                deferred.push_back({fi, port, s.jumpTo});
                pending.clear();
                break;
            case ImageNotFoundAction::Stop:
            case ImageNotFoundAction::Fail: {
                uint32_t n = add(NodeType::StopExecution);
                wirePendingTo(n);
                prop(n, "reason", Value::makeString(s.notFoundAction == ImageNotFoundAction::Fail
                                                        ? "image not found (fail)"
                                                        : "image not found"));
                if (s.notFoundAction == ImageNotFoundAction::Fail) {
                    rep.notes.push_back("ON NOT FOUND fail mapped to Stop Execution "
                                        "(reported as stopped, not failed)");
                    rep.warnings++;
                }
                pending.clear();
                break;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Graph -> Steps walker
// ---------------------------------------------------------------------------

struct Walker {
    const AutomationGraph& g;
    SimplifyReport& rep;
    std::unordered_set<uint32_t> emitted;

    Walker(const AutomationGraph& graph, SimplifyReport& r) : g(graph), rep(r) {}

    const GraphNode* node(uint32_t id) const { return g.findNode(id); }
    uint32_t follow(uint32_t nodeId, const char* portKey) const {
        const GraphNode* n = node(nodeId);
        if (!n) return 0;
        const Port* p = n->findOutputByKey(portKey);
        if (!p) return 0;
        for (const auto& c : g.connections)
            if (c.fromPort == p->id && c.kind == PortKind::Execution) return c.toNode;
        return 0;
    }
    int propInt(const GraphNode& n, const char* key, int def = 0) const {
        auto it = n.props.find(key);
        return it != n.props.end() ? static_cast<int>(it->second.asInt()) : def;
    }
    float propFloat(const GraphNode& n, const char* key, float def = 0.0f) const {
        auto it = n.props.find(key);
        return it != n.props.end() ? static_cast<float>(it->second.asFloat()) : def;
    }
    std::string propStr(const GraphNode& n, const char* key) const {
        auto it = n.props.find(key);
        return it != n.props.end() ? it->second.asString() : std::string{};
    }
    Vec2 propCoord(const GraphNode& n, const char* key) const {
        auto it = n.props.find(key);
        return it != n.props.end() ? it->second.asCoord() : Vec2{0.5f, 0.5f};
    }
    void warn(std::string t) { rep.warnings.push_back(std::move(t)); }

    // Emits steps following the exec chain until a dead end, an already-emitted
    // node, or a terminal node. Returns the node where the chain stopped so the
    // caller can detect a join point (0 = fully consumed).
    uint32_t chain(uint32_t id, std::vector<Step>& out) {
        while (id && node(id) && !emitted.count(id)) {
            emitted.insert(id);
            const GraphNode& n = *node(id);
            id = emitNode(n, out);
        }
        return id;
    }

    // Emits one node; returns the next node id on the main line.
    uint32_t emitNode(const GraphNode& n, std::vector<Step>& out) {
        auto mk = [&](StepType t) -> Step& {
            out.emplace_back();
            out.back().type = t;
            out.back().enabled = n.enabled;
            return out.back();
        };
        switch (n.type) {
            case NodeType::Start: return follow(n.id, "out");
            case NodeType::Delay: {
                Step& s = mk(StepType::Wait);
                s.ms = propInt(n, "duration", 250) * (propInt(n, "unit", 0) == 1 ? 1000 : 1);
                return follow(n.id, "out");
            }
            case NodeType::Wait: {
                Step& s = mk(StepType::Wait);
                s.ms = propInt(n, "ms", 100);
                return follow(n.id, "out");
            }
            case NodeType::Click: {
                Step& s = mk(StepType::Tap);
                s.a = propCoord(n, "point");
                s.ms = propInt(n, "holdMs", 40);
                return follow(n.id, "out");
            }
            case NodeType::DoubleClick: {
                warn("Double Click approximated as two Taps");
                Step& s = mk(StepType::Tap);
                s.a = propCoord(n, "point");
                Step& s2 = mk(StepType::Tap);
                s2 = s;
                return follow(n.id, "out");
            }
            case NodeType::LongPress: {
                Step& s = mk(StepType::LongPress);
                s.a = propCoord(n, "point");
                s.ms = propInt(n, "duration", 500);
                s.finger = propInt(n, "finger", 0);
                return follow(n.id, "out");
            }
            case NodeType::PressDown: {
                Step& s = mk(StepType::PressDown);
                s.a = propCoord(n, "point");
                s.finger = propInt(n, "finger", 0);
                return follow(n.id, "out");
            }
            case NodeType::Release: {
                Step& s = mk(StepType::Release);
                s.finger = propInt(n, "finger", 0);
                return follow(n.id, "out");
            }
            case NodeType::Slide: {
                Step& s = mk(StepType::Swipe);
                s.a = propCoord(n, "from");
                s.b = propCoord(n, "to");
                s.ms = propInt(n, "duration", 220);
                s.finger = propInt(n, "finger", 0);
                return follow(n.id, "out");
            }
            case NodeType::KeyPress: {
                Step& s = mk(StepType::Key);
                s.key = propInt(n, "key", 0x20);
                const bool hold = n.getProp("holdOnly", Value::makeBool(false)).asBool();
                s.keyMode = hold ? KeyMode::Down : KeyMode::Press;
                s.ms = propInt(n, "holdMs", 45);
                return follow(n.id, "out");
            }
            case NodeType::KeyRelease: {
                Step& s = mk(StepType::Key);
                s.key = propInt(n, "key", 0x20);
                s.keyMode = KeyMode::Up;
                return follow(n.id, "out");
            }
            case NodeType::RunMacro: {
                Step& s = mk(StepType::RunMacro);
                s.macro = propInt(n, "macroIndex", -1);
                return follow(n.id, "out");
            }
            case NodeType::CommentNode: {
                Step& s = mk(StepType::Comment);
                s.text = propStr(n, "text");
                return follow(n.id, "out");
            }
            case NodeType::JumpTarget: {
                Step& s = mk(StepType::Comment);
                s.text = "#" + propStr(n, "label");
                warn("Jump Target anchor exported as comment marker");
                return follow(n.id, "out");
            }
            case NodeType::StopExecution: {
                mk(StepType::Stop);
                return 0;
            }
            case NodeType::End: {
                mk(StepType::Stop);
                return 0;
            }
            case NodeType::Break: {
                mk(StepType::Break);
                return 0;  // Break jumps to the loop's completed port — handled by Loop emit
            }
            case NodeType::Continue: {
                mk(StepType::Comment).text = "continue (approximated)";
                warn("Continue node cannot be expressed in Simple Mode — left as comment");
                return 0;
            }
            case NodeType::CheckColorZone: {
                Step& s = mk(StepType::If);
                s.zone = propInt(n, "zoneIndex", 0);
                s.negate = n.getProp("negate", Value::makeBool(false)).asBool();
                return emitBranches(n, out, {{"present_exec", &s.then}, {"absent_exec", &s.otherwise}});
            }
            case NodeType::If: {
                Step& s = mk(StepType::If);
                s.zone = -1;
                s.image = -1;
                warn("If node condition is data-driven — exported without the condition pin");
                return emitBranches(n, out, {{"true", &s.then}, {"false", &s.otherwise}});
            }
            case NodeType::ImageFound:
            case NodeType::ImageNotFound: {
                Step& s = mk(StepType::If);
                s.negate = n.type == NodeType::ImageNotFound;
                warn("Image Found/Not Found router exported as plain If");
                return emitBranches(n, out, {{"yes", &s.then}, {"no", &s.otherwise}});
            }
            case NodeType::ColorFound:
            case NodeType::ColorNotFound: {
                Step& s = mk(StepType::If);
                s.zone = propInt(n, "zoneIndex", -1);
                s.negate = n.type == NodeType::ColorNotFound;
                return emitBranches(n, out, {{"yes", &s.then}, {"no", &s.otherwise}});
            }
            case NodeType::FindImage: {
                Step& s = mk(StepType::ImageRecognize);
                s.image = propInt(n, "imageIndex", -1);
                s.ms = propInt(n, "timeoutMs", 0);
                s.thresholdPct = propInt(n, "thresholdPct", 0);
                s.regionMode = static_cast<ImageRegionMode>(propInt(n, "regionMode", 0));
                s.region = {propFloat(n, "regionX"), propFloat(n, "regionY"),
                            propFloat(n, "regionW", 1.0f), propFloat(n, "regionH", 1.0f)};
                s.intervalMs = propInt(n, "intervalMs", 0);
                s.retryCount = propInt(n, "maxAttempts", 1);
                s.matchPolicy = static_cast<MatchPolicy>(propInt(n, "matchPolicy", 0));
                s.continuous = n.getProp("continuous", Value::makeBool(false)).asBool();
                s.foundAction = ImageFoundAction::RunSteps;
                s.notFoundAction = ImageNotFoundAction::RunSteps;
                return emitBranches(n, out, {{"found_exec", &s.then}, {"not_found_exec", &s.otherwise}});
            }
            case NodeType::WaitForImage: {
                Step& s = mk(StepType::ImageRecognize);
                s.image = propInt(n, "imageIndex", -1);
                s.ms = propInt(n, "timeoutMs", 5000);
                s.intervalMs = propInt(n, "intervalMs", 120);
                s.thresholdPct = propInt(n, "thresholdPct", 0);
                s.foundAction = ImageFoundAction::RunSteps;
                s.notFoundAction = ImageNotFoundAction::RunSteps;
                warn("Wait For Image exported as Image Recognize (timeout branch approximated)");
                return emitBranches(n, out, {{"found_exec", &s.then}, {"timeout_exec", &s.otherwise}});
            }
            case NodeType::Loop: {
                Step& s = mk(StepType::Loop);
                s.loopMode = LoopMode::Count;
                s.count = propInt(n, "count", 3);
                if (n.getProp("infinite", Value::makeBool(false)).asBool()) {
                    s.count = kMaxLoopCount;
                    warn("Infinite Loop exported with the Simple-Mode iteration cap");
                }
                return emitBranches(n, out, {{"body", &s.then}}, "completed");
            }
            case NodeType::ForLoop: {
                Step& s = mk(StepType::Loop);
                const int64_t first = propInt(n, "first", 0), last = propInt(n, "last", 4),
                              step = propInt(n, "step", 1);
                s.count = step != 0 ? static_cast<int>(std::abs(last - first) / std::abs(step)) + 1 : 0;
                s.loopMode = LoopMode::Count;
                warn("For Loop exported as Count loop — the index variable is not preserved");
                return emitBranches(n, out, {{"body", &s.then}}, "completed");
            }
            case NodeType::While: {
                Step& s = mk(StepType::Loop);
                s.loopMode = LoopMode::While;
                s.count = propInt(n, "maxIterations", 1000);
                warn("While node exported — its data-driven condition is not representable");
                return emitBranches(n, out, {{"body", &s.then}}, "completed");
            }
            case NodeType::Sequence: {
                for (int i = 0; i <= 2; ++i) {
                    const std::string key = "then" + std::to_string(i);
                    if (uint32_t t = follow(n.id, key.c_str())) {
                        std::vector<Step> branch;
                        chain(t, branch);
                        out.insert(out.end(), std::make_move_iterator(branch.begin()),
                                   std::make_move_iterator(branch.end()));
                    }
                }
                return 0;
            }
            default: {
                rep.nodesDropped++;
                warn(std::string("node '") + nodeTypeKey(n.type) +
                     "' has no Simple-Mode equivalent — dropped");
                return follow(n.id, "out");
            }
        }
    }

    // Emits a branching node: each exec-out gets its own nested step list, then
    // control resumes at the join point when every branch reconverges on it.
    uint32_t emitBranches(const GraphNode& n, std::vector<Step>& out,
                          std::initializer_list<std::pair<const char*, std::vector<Step>*>> branches,
                          const char* joinPort = nullptr) {
        std::vector<uint32_t> stops;
        for (auto& [port, list] : branches) {
            if (uint32_t t = follow(n.id, port)) stops.push_back(chain(t, *list));
            else stops.push_back(0);
        }
        if (joinPort) {
            if (uint32_t j = follow(n.id, joinPort)) stops.push_back(chain(j, out));
        }
        // Continue where all live branches reconverge; otherwise warn.
        uint32_t join = 0;
        bool same = true;
        for (uint32_t s : stops) {
            if (!s) continue;
            if (!join) join = s;
            else if (join != s) same = false;
        }
        if (!same) warn("branches do not reconverge — some trailing steps may be missing");
        return join;
    }
};

}  // namespace

// ---------------------------------------------------------------------------

ConversionReport macroToGraph(const Macro& m, const Automation& a) {
    ConversionReport rep;
    rep.graph = makeStarterGraph(m.name.empty() ? "Converted Macro" : m.name + " (Studio)");
    rep.graph.metadata.sourceLegacyMacro = m.name;
    rep.graph.metadata.triggerKey = m.key;
    rep.graph.metadata.triggerKey2 = m.key2;
    rep.graph.metadata.maxDurationMs = m.maxDurationMs;
    rep.graph.metadata.description = "Converted from Simple-Mode macro \"" + m.name + "\"";

    Builder b(rep, a, rep.graph);
    // Anchor every top-level step's first node after the Start output.
    uint32_t startId = 0;
    for (const auto& n : rep.graph.nodes)
        if (n.type == NodeType::Start) startId = n.id;
    if (!startId) startId = b.add(NodeType::Start);
    b.pending = {{startId, "out"}};
    b.chain(m.steps);
    if (!b.pending.empty()) {
        uint32_t end = b.add(NodeType::End);
        b.wirePendingTo(end);
    }
    rep.graph.metadata.conversionNotes = rep.notes;
    rep.graph.autoArrange();
    return rep;
}

SimplifyReport graphToMacroSteps(const AutomationGraph& g) {
    SimplifyReport rep;
    Walker w(g, rep);
    uint32_t startId = 0;
    for (const auto& n : g.nodes)
        if (n.type == NodeType::Start) startId = n.id;
    if (!startId) {
        rep.warnings.push_back("graph has no Start node");
        return rep;
    }
    w.emitted.insert(startId);
    w.chain(w.follow(startId, "out"), rep.steps);
    for (const auto& n : g.nodes)
        if (!w.emitted.count(n.id)) rep.nodesDropped++;
    return rep;
}

}  // namespace wasd::studio
