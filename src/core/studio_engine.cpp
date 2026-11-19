// NEXPLAY Automation Studio — graph execution engine implementation.
#include "core/studio_engine.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace wasd::studio {
namespace {

// Max nodes processed per tick() call; keeps the UI responsive even on
// pathological graphs (budget guard also applies across ticks).
constexpr int kTickSlice = 512;

const Port* portOf(const AutomationGraph& g, uint32_t nodeId, const char* key, PortDir dir) {
    const GraphNode* n = g.findNode(nodeId);
    if (!n) return nullptr;
    return dir == PortDir::Input ? n->findInputByKey(key) : n->findOutputByKey(key);
}

// Short human-readable preview of a Value (console + wire tooltips).
std::string previewOf(const Value& v) {
    char buf[96];
    switch (v.type) {
        case ValueType::Boolean: return v.b ? "true" : "false";
        case ValueType::Integer: std::snprintf(buf, sizeof(buf), "%lld", (long long)v.i); return buf;
        case ValueType::Float: std::snprintf(buf, sizeof(buf), "%.3f", v.f); return buf;
        case ValueType::String: return v.s.empty() ? "\"\"" : ("\"" + v.s.substr(0, 24) + "\"");
        case ValueType::Coordinate:
            std::snprintf(buf, sizeof(buf), "(%.3f, %.3f)", v.v.x, v.v.y); return buf;
        case ValueType::Vector2: std::snprintf(buf, sizeof(buf), "<%.3f, %.3f>", v.v.x, v.v.y); return buf;
        case ValueType::Color:
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", v.color.r, v.color.g, v.color.b); return buf;
        case ValueType::Image:
            std::snprintf(buf, sizeof(buf), "image[%d]", v.image.refIndex); return buf;
        case ValueType::ImageResult:
            std::snprintf(buf, sizeof(buf), "result(%s %.2f)", v.imageResult.found ? "found" : "miss",
                          v.imageResult.similarity);
            return buf;
        case ValueType::Array: std::snprintf(buf, sizeof(buf), "array[%zu]", v.arr.size()); return buf;
        case ValueType::Object: std::snprintf(buf, sizeof(buf), "object{%zu}", v.obj.size()); return buf;
        case ValueType::Any: return "any";
    }
    return "?";
}

// Reads x/y/w/h fields out of an Object value produced by Select Search Region.
Rect rectFromObject(const Value& v) {
    Rect r{0.0f, 0.0f, 1.0f, 1.0f};
    if (v.type != ValueType::Object) return r;
    auto f = [&](const char* k, float& out) {
        auto it = v.obj.find(k);
        if (it != v.obj.end()) out = (float)it->second.asFloat();
    };
    f("x", r.x); f("y", r.y); f("w", r.w); f("h", r.h);
    return r;
}

}  // namespace

// ------------------------------------------------------------------ helpers

int GraphRunner::propInt(const GraphNode& n, const char* key, int def) const {
    return (int)n.getProp(key, Value::makeInt(def)).asInt();
}
float GraphRunner::propFloat(const GraphNode& n, const char* key, float def) const {
    return (float)n.getProp(key, Value::makeFloat(def)).asFloat();
}
bool GraphRunner::propBool(const GraphNode& n, const char* key, bool def) const {
    return n.getProp(key, Value::makeBool(def)).asBool();
}
std::string GraphRunner::propStr(const GraphNode& n, const char* key, const char* def) const {
    Value v = n.getProp(key, Value::makeString(def));
    return v.asString();
}
Vec2 GraphRunner::pointOf(const GraphNode& n, const char* key) { return evalIn(n, key).asCoord(); }
int GraphRunner::fingerOf(const GraphNode& n) const { return std::clamp(propInt(n, "finger", 0), 0, kMaxFingers - 1); }

void GraphRunner::emitTouch(int action, int fi, Vec2 p) {
    fi = std::clamp(fi, 0, kMaxFingers - 1);
    if (action == 0) fingerDown_[fi] = true;
    if (action == 1) fingerDown_[fi] = false;
    if (host_.touch) host_.touch(action, fi, p);
}
void GraphRunner::emitKey(int vk, bool down) {
    if (down) keysDown_.insert(vk); else keysDown_.erase(vk);
    if (host_.key) host_.key(vk, down);
}
void GraphRunner::emitLog(int level, const std::string& text, uint32_t nodeId) {
    int64_t us = lastTickUs_ > startUs_ ? lastTickUs_ - startUs_ : 0;
    events_.push_back({us, level, nodeId, text});
    if (events_.size() > 4000) events_.erase(events_.begin(), events_.begin() + 1000);
    if (host_.log) host_.log(level, text);
}
void GraphRunner::markState(uint32_t nodeId, NodeRunState s) { nodeStates_[nodeId] = s; }
void GraphRunner::setOut(const AutomationGraph& g, uint32_t nodeId, const char* key, Value v) {
    outputs_[rtKey(g, nodeId)][key] = std::move(v);
}
uint64_t GraphRunner::rtKey(const AutomationGraph& g, uint32_t nodeId) const {
    // Node ids are only unique within their own graph — hash graph identity in.
    return (uint64_t)(uintptr_t)&g * 2654435761ull + nodeId;
}

// ------------------------------------------------------------------ lifecycle

bool GraphRunner::start(const Automation& a, const AutomationGraph& g, int64_t nowUs, uint32_t entryNodeId) {
    cancel();  // stop any previous run silently
    automation_ = a;
    graph_ = g;
    rt_.clear(); outputs_.clear(); memo_.clear(); wireValues_.clear();
    nodeStates_.clear(); vars_.clear(); events_.clear(); bpHit_.clear();
    callStack_.clear(); seqStack_.clear(); loopStack_.clear();
    executed_ = 0; epoch_++;
    paused_ = stepOnce_ = false;
    std::fill(std::begin(fingerDown_), std::end(fingerDown_), false);
    keysDown_.clear();

    // seed runtime variables from the graph declaration
    for (const GraphVariable& v : graph_.variables) vars_[v.name] = v.initialValue;
    maxDurationUs_ = int64_t(std::max(100, graph_.metadata.maxDurationMs)) * 1000;
    maxStepsBudget_ = std::max(100, graph_.metadata.maxStepsBudget);

    const GraphNode* start = nullptr;
    if (entryNodeId)
        for (const GraphNode& n : graph_.nodes)
            if (n.id == entryNodeId && n.type == NodeType::Start) { start = &n; break; }
    if (!start)
        for (const GraphNode& n : graph_.nodes)
            if (n.type == NodeType::Start) { start = &n; break; }
    if (!start) {
        message_ = "graph has no Start node";
        emitLog(2, message_);
        state_ = RunState::Failed;
        return false;
    }
    cur_ = {&graph_, start->id};
    startUs_ = nowUs;
    state_ = RunState::Running;
    message_.clear();
    emitLog(0, "Graph \"" + graph_.name + "\" started", start->id);
    return true;
}

void GraphRunner::cancel(const char* reason) {
    if (!active()) { state_ = RunState::Idle; return; }
    emitLog(1, std::string("Cancelled: ") + reason);
    finish(RunState::Cancelled, reason);
}

void GraphRunner::finish(RunState s, const std::string& msg) {
    if (!msg.empty()) message_ = msg;
    releaseAll();
    state_ = s;
    cur_ = {};
    if (!msg.empty()) emitLog(s == RunState::Failed ? 2 : 0, msg);
    emitLog(0, "Graph finished: " + std::string(runStateName(s)));
}

void GraphRunner::fail(const std::string& why) {
    if (cur_.node) markState(cur_.node, NodeRunState::Error);
    emitLog(2, "Error: " + why, cur_.node);
    finish(RunState::Failed, why);
}

void GraphRunner::releaseAll() {
    for (int i = 0; i < kMaxFingers; ++i)
        if (fingerDown_[i] && host_.touch) host_.touch(1, i, Vec2{0.5f, 0.5f});
    std::fill(std::begin(fingerDown_), std::end(fingerDown_), false);
    for (int vk : keysDown_)
        if (host_.key) host_.key(vk, false);
    keysDown_.clear();
}

// ------------------------------------------------------------------ stepping

void GraphRunner::tick(int64_t nowUs) {
    if (!active()) return;
    lastTickUs_ = nowUs;
    if (nowUs - startUs_ > maxDurationUs_) {
        fail("execution timeout (" + std::to_string(graph_.metadata.maxDurationMs) + " ms)");
        return;
    }
    stepSlice(nowUs);
}

void GraphRunner::stepSlice(int64_t now) {
    int guard = kTickSlice;
    while (guard-- > 0 && active() && !paused_) {
        if (cur_.node == 0) {
            if (!popContinuation()) {
                finish(RunState::Completed);
                return;
            }
            continue;
        }
        const GraphNode* n = cur_.g->findNode(cur_.node);
        if (!n) { cur_.node = 0; continue; }
        if (executed_ >= maxStepsBudget_) { fail("step budget exceeded"); return; }
        if (debug_ && n->breakpoint && !bpHit_.count(n->id)) {
            bpHit_.insert(n->id);
            paused_ = true;
            emitLog(0, "Breakpoint hit on \"" + (n->title.empty() ? std::string(nodeTypeKey(n->type)) : n->title) + "\"", n->id);
            return;
        }
        if (!n->enabled) {
            markState(n->id, NodeRunState::Skipped);
            advanceDisabled(*n);
            executed_++;
            continue;
        }
        state_ = RunState::Running;
        memo_.clear();
        markState(n->id, NodeRunState::Active);
        uint32_t executedNode = n->id;
        runNode(*cur_.g, *n, now);
        if (state_ == RunState::Waiting) {
            markState(executedNode, NodeRunState::Waiting);
            return;
        }
        if (!active()) return;
        markState(executedNode, NodeRunState::Done);
        executed_++;
        if (stepOnce_) { paused_ = true; stepOnce_ = false; return; }
    }
    if (guard <= 0) emitLog(1, "tick slice exhausted (possible busy loop)");
}

void GraphRunner::stepOnce() {
    if (paused_) { paused_ = false; stepOnce_ = true; }
}

uint32_t GraphRunner::followExecOut(const AutomationGraph& g, uint32_t nodeId, const char* portKey) const {
    const Port* p = portOf(g, nodeId, portKey, PortDir::Output);
    if (!p) return 0;
    for (const GraphConnection& c : g.connections)
        if (c.fromPort == p->id && c.kind == PortKind::Execution) return c.toNode;
    return 0;
}

void GraphRunner::advanceOut(const GraphNode& n, const char* portKey) {
    cur_.node = followExecOut(*cur_.g, n.id, portKey);
}

// A disabled node passes control through its most natural continuation port.
void GraphRunner::advanceDisabled(const GraphNode& n) {
    static const char* kPass[] = {"out", "true", "yes", "body", "found_exec", "then0"};
    for (const char* key : kPass) {
        uint32_t t = followExecOut(*cur_.g, n.id, key);
        if (t) { cur_.node = t; return; }
    }
    cur_.node = 0;
}

bool GraphRunner::popContinuation() {
    // 1) pending Sequence branches in the current graph
    while (!seqStack_.empty() && seqStack_.back().g == cur_.g) {
        SeqCont& sc = seqStack_.back();
        if (sc.nextBranch > 2) { seqStack_.pop_back(); continue; }
        std::string key = "then" + std::to_string(sc.nextBranch);
        sc.nextBranch++;
        uint32_t t = followExecOut(*sc.g, sc.node, key.c_str());
        if (t) { cur_.node = t; return true; }
    }
    // 2) implicit loop back-edge: body chain ended inside an iterating loop
    if (!loopStack_.empty() && loopStack_.back().g == cur_.g) {
        cur_.node = loopStack_.back().node;
        return true;
    }
    // 3) returning from a Function / Subgraph call
    if (subDepth_ > 0 && !callStack_.empty()) {
        cur_ = callStack_.back();
        callStack_.pop_back();
        subDepth_--;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ data pull

Value GraphRunner::evalIn(const GraphNode& n, const char* key) {
    const Port* p = n.findInputByKey(key);
    if (!p) {
        auto it = n.props.find(key);
        return it != n.props.end() ? it->second : Value{};
    }
    for (const GraphConnection& c : cur_.g->connections) {
        if (c.toPort != p->id || c.kind != PortKind::Data) continue;
        const GraphNode* pn = nullptr;
        const Port* pp = nullptr;
        if (!cur_.g->findPortOwner(c.fromPort, pn, pp) || !pn || !pp) break;
        Value v = evalOut(*cur_.g, pn->id, pp->key);
        wireValues_[c.id] = previewOf(v);
        return coerceValue(v, p->dataType);
    }
    auto it = n.props.find(key);
    const Value& src = it != n.props.end() ? it->second : p->defaultValue;
    return coerceValue(src, p->dataType);
}

Value GraphRunner::evalOut(const AutomationGraph& g, uint32_t nodeId, const std::string& portKey) {
    const GraphNode* pn = g.findNode(nodeId);
    if (!pn) return {};
    const NodeTypeSpec* spec = findNodeSpec(pn->type, pn->customTypeKey);
    uint64_t key = rtKey(g, nodeId);
    if (spec && spec->pureData) {
        auto mit = memo_.find(key);
        if (mit == memo_.end()) mit = memo_.emplace(key, computePure(*pn)).first;
        auto it = mit->second.find(portKey);
        if (it != mit->second.end()) return it->second;
        const Port* p = portOf(g, nodeId, portKey.c_str(), PortDir::Output);
        return p ? p->defaultValue : Value{};
    }
    auto oit = outputs_.find(key);
    if (oit != outputs_.end()) {
        auto it = oit->second.find(portKey);
        if (it != oit->second.end()) return it->second;
    }
    const Port* p = portOf(g, nodeId, portKey.c_str(), PortDir::Output);
    return p ? p->defaultValue : Value{};
}

NodeRunState GraphRunner::nodeState(uint32_t nodeId) const {
    auto it = nodeStates_.find(nodeId);
    return it != nodeStates_.end() ? it->second : NodeRunState::Idle;
}
const Value* GraphRunner::nodeOutput(uint32_t nodeId, const std::string& portKey) const {
    auto oit = outputs_.find(rtKey(graph_, nodeId));
    if (oit == outputs_.end()) return nullptr;
    auto it = oit->second.find(portKey);
    return it != oit->second.end() ? &it->second : nullptr;
}
std::string GraphRunner::wireValue(uint32_t connId) const {
    auto it = wireValues_.find(connId);
    return it != wireValues_.end() ? it->second : std::string{};
}

// ------------------------------------------------------------------ pure nodes

std::unordered_map<std::string, Value> GraphRunner::computePure(const GraphNode& n) {
    std::unordered_map<std::string, Value> out;
    auto num = [&](const char* k) { return evalIn(n, k).asFloat(); };
    auto tf = [&](const char* k) { return evalIn(n, k).asBool(); };
    switch (n.type) {
        case NodeType::LogicAnd: out["result"] = Value::makeBool(tf("a") && tf("b")); break;
        case NodeType::LogicOr: out["result"] = Value::makeBool(tf("a") || tf("b")); break;
        case NodeType::LogicNot: out["result"] = Value::makeBool(!tf("value")); break;
        case NodeType::Equal: out["result"] = Value::makeBool(evalIn(n, "a") == evalIn(n, "b")); break;
        case NodeType::NotEqual: out["result"] = Value::makeBool(!(evalIn(n, "a") == evalIn(n, "b"))); break;
        case NodeType::GreaterThan: out["result"] = Value::makeBool(num("a") > num("b")); break;
        case NodeType::LessThan: out["result"] = Value::makeBool(num("a") < num("b")); break;
        case NodeType::IsEmpty: out["empty"] = Value::makeBool(evalIn(n, "value").isEmpty()); break;
        case NodeType::MathAdd: out["result"] = Value::makeFloat(num("a") + num("b")); break;
        case NodeType::MathSubtract: out["result"] = Value::makeFloat(num("a") - num("b")); break;
        case NodeType::MathMultiply: out["result"] = Value::makeFloat(num("a") * num("b")); break;
        case NodeType::MathDivide: {
            double b = num("b");
            out["result"] = Value::makeFloat(std::abs(b) < 1e-9 ? 0.0 : num("a") / b);
            break;
        }
        case NodeType::MathClamp:
            out["result"] = Value::makeFloat(std::clamp(num("value"), num("min"), num("max")));
            break;
        case NodeType::MathMin: out["result"] = Value::makeFloat(std::min(num("a"), num("b"))); break;
        case NodeType::MathMax: out["result"] = Value::makeFloat(std::max(num("a"), num("b"))); break;
        case NodeType::RandomValue: {
            double lo = std::min(num("min"), num("max")), hi = std::max(num("min"), num("max"));
            std::uniform_real_distribution<double> d(lo, hi);
            double r = d(rng_);
            out["float_val"] = Value::makeFloat(r);
            out["int_val"] = Value::makeInt((int64_t)std::llround(r));
            break;
        }
        case NodeType::GetVariable: {
            const std::string name = propStr(n, "name", "counter");
            auto it = vars_.find(name);
            out["value"] = it != vars_.end() ? it->second : Value::makeInt(0);
            break;
        }
        case NodeType::CompareSimilarity: {
            double sim = num("similarity"), thr = num("threshold");
            out["meets"] = Value::makeBool(sim >= thr);
            out["delta"] = Value::makeFloat(sim - thr);
            break;
        }
        case NodeType::GetImageCoordinates: {
            Value rv = evalIn(n, "result");
            Vec2 pt = rv.imageResult.coordinates;
            const Rect& box = rv.imageResult.matchedBox;
            if (box.w > 0.0001f && box.h > 0.0001f) {
                Vec2 ofs = evalIn(n, "offset").asVec2();
                pt = {box.x + ofs.x * box.w, box.y + ofs.y * box.h};
            }
            out["coordinates"] = Value::makeCoord(pt);
            out["found"] = Value::makeBool(rv.imageResult.found);
            break;
        }
        case NodeType::SelectSearchRegion: {
            std::map<std::string, Value> m;
            m["x"] = Value::makeFloat(propFloat(n, "x"));
            m["y"] = Value::makeFloat(propFloat(n, "y"));
            m["w"] = Value::makeFloat(propFloat(n, "w", 1.0f));
            m["h"] = Value::makeFloat(propFloat(n, "h", 1.0f));
            out["region"] = Value::makeObject(std::move(m));
            break;
        }
        case NodeType::ImageSearchResult: {
            const ImageResultVal& r = evalIn(n, "result").imageResult;
            out["found"] = Value::makeBool(r.found);
            out["similarity"] = Value::makeFloat(r.similarity);
            out["coordinates"] = Value::makeCoord(r.coordinates);
            out["duration"] = Value::makeFloat(r.searchDurationMs);
            std::map<std::string, Value> m;
            m["x"] = Value::makeFloat(r.searchRegion.x); m["y"] = Value::makeFloat(r.searchRegion.y);
            m["w"] = Value::makeFloat(r.searchRegion.w); m["h"] = Value::makeFloat(r.searchRegion.h);
            out["region"] = Value::makeObject(std::move(m));
            break;
        }
        case NodeType::CompareColor: {
            ColorVal a = evalIn(n, "a").color, b = evalIn(n, "b").color;
            float de = deltaE(rgbToLab(a.r, a.g, a.b), rgbToLab(b.r, b.g, b.b));
            float tol = std::max({a.tolerance, b.tolerance, propFloat(n, "tolerance", 12.0f)});
            out["matched"] = Value::makeBool(de <= tol);
            out["similarity"] = Value::makeFloat(std::clamp(1.0 - de / 50.0, 0.0, 1.0));
            out["deltaE"] = Value::makeFloat(de);
            break;
        }
        case NodeType::ColorSimilarity: {
            ColorVal a = evalIn(n, "a").color, b = evalIn(n, "b").color;
            float de = deltaE(rgbToLab(a.r, a.g, a.b), rgbToLab(b.r, b.g, b.b));
            out["similarity"] = Value::makeFloat(std::clamp(1.0 - de / 50.0, 0.0, 1.0));
            break;
        }
        default: break;
    }
    return out;
}

// ------------------------------------------------------------------ exec nodes

void GraphRunner::runNode(const AutomationGraph& g, const GraphNode& n, int64_t now) {
    NodeRt& rt = rt_[rtKey(*cur_.g, n.id)];
    switch (n.type) {
        case NodeType::Start: advanceOut(n, "out"); break;

        case NodeType::End:
            if (subDepth_ > 0 && !callStack_.empty()) {
                cur_ = callStack_.back();
                callStack_.pop_back();
                subDepth_--;
            } else {
                finish(RunState::Completed, propStr(n, "statusText", "Completed"));
            }
            break;

        case NodeType::Delay:
        case NodeType::Wait: {
            if (rt.untilUs == 0) {
                int64_t ms;
                if (n.type == NodeType::Wait) {
                    ms = evalIn(n, "ms").asInt();
                } else {
                    ms = evalIn(n, "duration").asInt();
                    if (propInt(n, "unit", 0) == 1) ms *= 1000;
                    int64_t jit = propInt(n, "randomJitterMs", 0);
                    if (jit > 0) {
                        std::uniform_int_distribution<int64_t> d(-jit, jit);
                        ms += d(rng_);
                    }
                }
                rt.untilUs = now + std::max<int64_t>(0, ms) * 1000;
            }
            if (now < rt.untilUs) { state_ = RunState::Waiting; return; }
            rt = {};
            advanceOut(n);
            break;
        }

        case NodeType::Loop: {
            bool infinite = propBool(n, "infinite", false);
            int pauseMs = propInt(n, "pauseMs", 0);
            if (rt.phase == 0) {  // first entry
                int64_t count = evalIn(n, "count").asInt();
                rt.aux = count;
                if (!infinite && count <= 0) { advanceOut(n, "completed"); break; }
                rt.phase = 1;
                rt.iter = 0;
                loopStack_.push_back({cur_.g, n.id, seqStack_.size()});
                setOut(*cur_.g, n.id, "index", Value::makeInt(0));
                advanceOut(n, "body");
            } else {  // back-edge: previous body iteration ended
                if (pauseMs > 0 && rt.untilUs == 0) {
                    rt.untilUs = now + int64_t(pauseMs) * 1000;
                    state_ = RunState::Waiting;
                    return;
                }
                if (rt.untilUs != 0 && now < rt.untilUs) { state_ = RunState::Waiting; return; }
                rt.untilUs = 0;
                rt.iter++;
                if (!infinite && rt.iter >= rt.aux) {
                    rt.phase = 0;
                    loopStack_.pop_back();
                    advanceOut(n, "completed");
                } else {
                    setOut(*cur_.g, n.id, "index", Value::makeInt(rt.iter));
                    advanceOut(n, "body");
                }
            }
            break;
        }

        case NodeType::ForLoop: {
            if (rt.phase == 0) {
                rt.iter = evalIn(n, "first").asInt();
                rt.aux = evalIn(n, "last").asInt();
                rt.untilUs = 0;
                int64_t step = evalIn(n, "step").asInt();
                rt.nextPollUs = step;
                if (step == 0) { fail("For Loop step is zero"); return; }
                if ((step > 0 && rt.iter > rt.aux) || (step < 0 && rt.iter < rt.aux)) {
                    advanceOut(n, "completed");
                    break;
                }
                rt.phase = 1;
                loopStack_.push_back({cur_.g, n.id, seqStack_.size()});
                setOut(*cur_.g, n.id, "index", Value::makeInt(rt.iter));
                advanceOut(n, "body");
            } else {
                rt.iter += rt.nextPollUs;
                if ((rt.nextPollUs > 0 && rt.iter > rt.aux) || (rt.nextPollUs < 0 && rt.iter < rt.aux)) {
                    rt.phase = 0;
                    loopStack_.pop_back();
                    advanceOut(n, "completed");
                } else {
                    setOut(*cur_.g, n.id, "index", Value::makeInt(rt.iter));
                    advanceOut(n, "body");
                }
            }
            break;
        }

        case NodeType::While: {
            int64_t maxIt = propInt(n, "maxIterations", 1000);
            int pauseMs = propInt(n, "pollDelayMs", 16);
            auto check = [&]() {
                if (rt.iter >= maxIt) return false;
                return evalIn(n, "condition").asBool();
            };
            if (rt.phase == 0) {
                if (!check()) { advanceOut(n, "completed"); break; }
                rt.phase = 1;
                loopStack_.push_back({cur_.g, n.id, seqStack_.size()});
                setOut(*cur_.g, n.id, "iteration", Value::makeInt(rt.iter));
                advanceOut(n, "body");
            } else {
                if (pauseMs > 0 && rt.untilUs == 0) {
                    rt.untilUs = now + int64_t(pauseMs) * 1000;
                    state_ = RunState::Waiting;
                    return;
                }
                if (rt.untilUs != 0 && now < rt.untilUs) { state_ = RunState::Waiting; return; }
                rt.untilUs = 0;
                rt.iter++;
                if (!check()) {
                    rt.phase = 0;
                    loopStack_.pop_back();
                    advanceOut(n, "completed");
                } else {
                    setOut(*cur_.g, n.id, "iteration", Value::makeInt(rt.iter));
                    advanceOut(n, "body");
                }
            }
            break;
        }

        case NodeType::Break: {
            if (loopStack_.empty()) {
                emitLog(1, "Break used outside a loop — stopping branch", n.id);
                cur_.node = 0;
            } else {
                uint32_t lp = loopStack_.back().node;
                rt_[rtKey(*cur_.g, lp)].phase = 0;
                seqStack_.resize(loopStack_.back().seqDepth);
                loopStack_.pop_back();
                cur_.node = followExecOut(g, lp, "completed");
            }
            break;
        }
        case NodeType::Continue: {
            if (loopStack_.empty()) {
                emitLog(1, "Continue used outside a loop — stopping branch", n.id);
                cur_.node = 0;
            } else {
                seqStack_.resize(loopStack_.back().seqDepth);
                cur_.node = loopStack_.back().node;
            }
            break;
        }

        case NodeType::If: {
            bool c = evalIn(n, "condition").asBool();
            if (propBool(n, "negate", false)) c = !c;
            advanceOut(n, c ? "true" : "false");
            break;
        }
        case NodeType::Else: advanceOut(n); break;

        case NodeType::Switch: {
            int64_t v = evalIn(n, "value").asInt();
            const char* key = "default";
            for (int i = 0; i <= 2; ++i) {
                if (v == propInt(n, ("case" + std::to_string(i) + "Val").c_str(), i)) {
                    key = i == 0 ? "case0" : i == 1 ? "case1" : "case2";
                    break;
                }
            }
            advanceOut(n, key);
            break;
        }

        case NodeType::Sequence:
            seqStack_.push_back({cur_.g, n.id, 1});
            advanceOut(n, "then0");
            break;

        case NodeType::Function:
        case NodeType::Subgraph: {
            const AutomationGraph* sub = nullptr;
            if (!n.subgraphId.empty()) {
                for (const auto& sg : g.subgraphs)
                    if (sg.id == n.subgraphId) { sub = &sg; break; }
            }
            if (!sub) {
                std::string want = n.type == NodeType::Function ? propStr(n, "functionName")
                                                              : propStr(n, "subgraphId");
                if (want.empty()) want = n.subgraphId;
                for (const auto& sg : g.subgraphs)
                    if (sg.name == want || sg.id == want) { sub = &sg; break; }
            }
            if (!sub) {
                emitLog(1, "Function/Subgraph target not found — skipping", n.id);
                advanceOut(n);
                break;
            }
            if (subDepth_ >= kMaxGraphDepth) { fail("subgraph depth exceeded"); return; }
            callStack_.push_back({cur_.g, followExecOut(g, n.id, "out")});
            subDepth_++;
            setOut(*cur_.g, n.id, "return", evalIn(n, "arg"));
            const GraphNode* sStart = nullptr;
            for (const auto& sn : sub->nodes)
                if (sn.type == NodeType::Start) { sStart = &sn; break; }
            if (!sStart) {
                emitLog(1, "subgraph has no Start node", n.id);
                cur_ = callStack_.back();
                callStack_.pop_back();
                subDepth_--;
                advanceOut(n);
                break;
            }
            cur_ = {sub, sStart->id};
            break;
        }

        // ---------------- input actions ----------------
        case NodeType::Click: {
            if (rt.phase == 0) {
                Vec2 p = pointOf(n, "point");
                emitTouch(0, fingerOf(n), p);
                rt.phase = 1;
                rt.untilUs = now + int64_t(std::max(15, propInt(n, "holdMs", 40))) * 1000;
                state_ = RunState::Waiting;
                return;
            }
            emitTouch(1, fingerOf(n), pointOf(n, "point"));
            rt = {};
            advanceOut(n);
            break;
        }
        case NodeType::DoubleClick: {
            Vec2 p = pointOf(n, "point");
            int gap = std::max(20, propInt(n, "gapMs", 75));
            switch (rt.phase) {
                case 0:
                    emitTouch(0, fingerOf(n), p);
                    emitTouch(1, fingerOf(n), p);
                    rt.phase = 1;
                    rt.untilUs = now + int64_t(gap) * 1000;
                    state_ = RunState::Waiting;
                    return;
                case 1:
                    if (now < rt.untilUs) { state_ = RunState::Waiting; return; }
                    emitTouch(0, fingerOf(n), p);
                    emitTouch(1, fingerOf(n), p);
                    rt = {};
                    advanceOut(n);
                    break;
            }
            break;
        }
        case NodeType::LongPress: {
            Vec2 p = pointOf(n, "point");
            if (rt.phase == 0) {
                emitTouch(0, fingerOf(n), p);
                rt.phase = 1;
                rt.untilUs = now + std::max<int64_t>(40, evalIn(n, "duration").asInt()) * 1000;
                state_ = RunState::Waiting;
                return;
            }
            if (now < rt.untilUs) { state_ = RunState::Waiting; return; }
            emitTouch(1, fingerOf(n), p);
            rt = {};
            advanceOut(n);
            break;
        }
        case NodeType::PressDown:
            emitTouch(0, fingerOf(n), pointOf(n, "point"));
            advanceOut(n);
            break;
        case NodeType::Release:
            emitTouch(1, fingerOf(n), Vec2{0.5f, 0.5f});
            advanceOut(n);
            break;
        case NodeType::Slide: {
            if (rt.phase == 0) {
                rt.from = evalIn(n, "from").asCoord();
                rt.to = evalIn(n, "to").asCoord();
                rt.aux = now;
                rt.untilUs = now + std::max<int64_t>(16, evalIn(n, "duration").asInt()) * 1000;
                emitTouch(0, fingerOf(n), rt.from);
                rt.phase = 1;
                state_ = RunState::Waiting;
                return;
            }
            if (now < rt.untilUs) {
                double t = double(now - rt.aux) / double(rt.untilUs - rt.aux);
                Vec2 m{rt.from.x + float(t) * (rt.to.x - rt.from.x),
                       rt.from.y + float(t) * (rt.to.y - rt.from.y)};
                emitTouch(2, fingerOf(n), m);
                state_ = RunState::Waiting;
                return;
            }
            emitTouch(1, fingerOf(n), rt.to);
            rt = {};
            advanceOut(n);
            break;
        }
        case NodeType::KeyPress: {
            int vk = propInt(n, "key", 0x20);
            if (propBool(n, "holdOnly", false)) {
                emitKey(vk, true);
                advanceOut(n);
                break;
            }
            if (rt.phase == 0) {
                emitKey(vk, true);
                rt.phase = 1;
                rt.untilUs = now + int64_t(std::max(15, propInt(n, "holdMs", 45))) * 1000;
                state_ = RunState::Waiting;
                return;
            }
            if (now < rt.untilUs) { state_ = RunState::Waiting; return; }
            emitKey(vk, false);
            rt = {};
            advanceOut(n);
            break;
        }
        case NodeType::KeyRelease:
            emitKey(propInt(n, "key", 0x20), false);
            advanceOut(n);
            break;

        // ---------------- image recognition ----------------
        case NodeType::CaptureScreenshot: {
            // Phase 7 wires a real capture slot; for now the node marks that a
            // capture was requested so downstream Image handles resolve.
            Value img = Value::makeImage(-1);
            img.image.captureSlot = 0;
            img.image.label = "capture";
            setOut(*cur_.g, n.id, "image", img);
            emitLog(0, "Screenshot captured", n.id);
            advanceOut(n);
            break;
        }
        case NodeType::FindImage: {
            int idx = -1;
            {
                Value iv = evalIn(n, "image");
                idx = iv.image.refIndex >= 0 ? iv.image.refIndex : propInt(n, "imageIndex", -1);
            }
            if (idx < 0 || idx >= (int)automation_.images.size()) {
                emitLog(1, "Find Image: invalid reference image", n.id);
                setOut(*cur_.g, n.id, "found", Value::makeBool(false));
                advanceOut(n, "not_found_exec");
                break;
            }
            int64_t timeoutMs = propInt(n, "timeoutMs", 0);
            int maxAttempts = propInt(n, "maxAttempts", 1);
            int64_t intervalMs = std::max(16, propInt(n, "intervalMs", 120));
            if (rt.phase == 0) {
                rt.phase = 1;
                rt.aux = 0;                                    // attempts
                rt.untilUs = timeoutMs > 0 ? now + timeoutMs * 1000 : 0;  // deadline
                rt.nextPollUs = 0;
            }
            if (now < rt.nextPollUs) { state_ = RunState::Waiting; return; }
            rt.nextPollUs = now + intervalMs * 1000;
            ImageQuery q;  // hoisted: the not-found emit path reads q.region too
            if (host_.imageScan) {
                const ImageRef& ref = automation_.images[idx];
                int rm = propInt(n, "regionMode", 0);
                // A wired Search Region port always wins over the props.
                bool regionOverride = false;
                if (const Port* rp = n.findInputByKey("region"))
                    for (const GraphConnection& c : g.connections)
                        if (c.toPort == rp->id && c.kind == PortKind::Data) regionOverride = true;
                Rect rg = rectFromObject(evalIn(n, "region"));
                if (regionOverride) {
                    q.region = rg;
                } else {
                    q.region = rm == 2 ? Rect{propFloat(n, "regionX"), propFloat(n, "regionY"),
                                              propFloat(n, "regionW", 1.0f), propFloat(n, "regionH", 1.0f)}
                                     : rm == 1 ? Rect{0, 0, 1, 1} : ref.region;
                }
                q.thresholdPct = propInt(n, "thresholdPct", 0) > 0 ? propInt(n, "thresholdPct", 85)
                                                                 : ref.thresholdPct;
                q.intervalMs = (int)intervalMs;
                q.wantMatches = propInt(n, "matchPolicy", 0) != 0;
                ImageScanResult r = host_.imageScan(idx, q);
                if (r.available && r.paramsMatch) {
                    rt.auxF = r.score;  // best score so far — surfaced on miss for diagnostics
                    if (r.found) {
                        ImageResultVal ir;
                        ir.found = true;
                        ir.similarity = r.score;
                        ir.coordinates = r.match.center;
                        ir.matchedBox = r.match.rect;
                        ir.searchRegion = q.region;
                        ir.searchDurationMs = float(r.scanUs) / 1000.0f;
                        ir.matchCount = r.matches.empty() ? (r.match.found ? 1 : 0) : (int)r.matches.size();
                        setOut(*cur_.g, n.id, "found", Value::makeBool(true));
                        setOut(*cur_.g, n.id, "similarity", Value::makeFloat(ir.similarity));
                        setOut(*cur_.g, n.id, "coordinates", Value::makeCoord(ir.coordinates));
                        setOut(*cur_.g, n.id, "duration", Value::makeFloat(ir.searchDurationMs));
                        std::map<std::string, Value> rm_;
                        rm_["x"] = Value::makeFloat(q.region.x); rm_["y"] = Value::makeFloat(q.region.y);
                        rm_["w"] = Value::makeFloat(q.region.w); rm_["h"] = Value::makeFloat(q.region.h);
                        setOut(*cur_.g, n.id, "out_region", Value::makeObject(std::move(rm_)));
                        setOut(*cur_.g, n.id, "result", Value::makeImageResult(ir));
                        rt = {};
                        advanceOut(n, "found_exec");
                        break;
                    }
                    rt.aux++;  // a real miss
                }
            } else {
                rt.aux++;
            }
            {
                bool timeout = rt.untilUs > 0 && now >= rt.untilUs;
                bool attemptsDone = maxAttempts > 0 && rt.aux >= maxAttempts;
                bool single = timeoutMs <= 0 && rt.aux >= 1;
                if (timeout || attemptsDone || single) {
                    ImageResultVal ir;
                    ir.found = false;
                    ir.similarity = rt.auxF;  // best sub-threshold score — helps tune Accuracy
                    ir.searchRegion = q.region;
                    setOut(*cur_.g, n.id, "found", Value::makeBool(false));
                    setOut(*cur_.g, n.id, "similarity", Value::makeFloat(ir.similarity));
                    setOut(*cur_.g, n.id, "result", Value::makeImageResult(ir));
                    rt = {};
                    advanceOut(n, "not_found_exec");
                    break;
                }
            }
            state_ = RunState::Waiting;
            return;
        }
        case NodeType::WaitForImage: {
            int idx = propInt(n, "imageIndex", -1);
            if (idx < 0 || idx >= (int)automation_.images.size()) {
                emitLog(1, "Wait For Image: invalid reference image", n.id);
                advanceOut(n, "timeout_exec");
                break;
            }
            int64_t timeoutMs = std::max<int64_t>(50, propInt(n, "timeoutMs", 5000));
            int64_t intervalMs = std::max(16, propInt(n, "intervalMs", 120));
            if (rt.phase == 0) {
                rt.phase = 1;
                rt.untilUs = now + timeoutMs * 1000;
                rt.nextPollUs = 0;
            }
            if (now < rt.nextPollUs) { state_ = RunState::Waiting; return; }
            rt.nextPollUs = now + intervalMs * 1000;
            bool found = false;
            if (host_.imageScan) {
                ImageQuery q;
                const ImageRef& ref = automation_.images[idx];
                q.region = ref.region;
                q.thresholdPct = propInt(n, "thresholdPct", 0) > 0 ? propInt(n, "thresholdPct", 85)
                                                                 : ref.thresholdPct;
                q.intervalMs = (int)intervalMs;
                ImageScanResult r = host_.imageScan(idx, q);
                if (r.available && r.paramsMatch && r.found) {
                    ImageResultVal ir;
                    ir.found = true;
                    ir.similarity = r.score;
                    ir.coordinates = r.match.center;
                    ir.matchedBox = r.match.rect;
                    ir.searchRegion = q.region;
                    ir.searchDurationMs = float(r.scanUs) / 1000.0f;
                    setOut(*cur_.g, n.id, "found", Value::makeBool(true));
                    setOut(*cur_.g, n.id, "similarity", Value::makeFloat(ir.similarity));
                    setOut(*cur_.g, n.id, "coordinates", Value::makeCoord(ir.coordinates));
                    setOut(*cur_.g, n.id, "result", Value::makeImageResult(ir));
                    found = true;
                }
            }
            if (found) {
                rt = {};
                advanceOut(n, "found_exec");
                break;
            }
            if (now >= rt.untilUs) {
                setOut(*cur_.g, n.id, "found", Value::makeBool(false));
                rt = {};
                advanceOut(n, "timeout_exec");
                break;
            }
            state_ = RunState::Waiting;
            return;
        }
        case NodeType::ImageFound:
        case NodeType::ImageNotFound: {
            bool f = evalIn(n, "result").imageResult.found;
            bool yes = n.type == NodeType::ImageFound ? f : !f;
            setOut(*cur_.g, n.id, n.type == NodeType::ImageFound ? "found" : "not_found", Value::makeBool(yes));
            advanceOut(n, yes ? "yes" : "no");
            break;
        }

        // ---------------- color recognition ----------------
        case NodeType::GetPixelColor: {
            if (!host_.pixelColor) {
                setOut(*cur_.g, n.id, "color", Value::defaultFor(ValueType::Color));
                emitLog(1, "Get Pixel Color: no sampling sink", n.id);
                advanceOut(n);
                break;
            }
            auto c = host_.pixelColor(pointOf(n, "point"));
            if (!c) { state_ = RunState::Waiting; return; }
            setOut(*cur_.g, n.id, "color", Value::makeColor(c->r, c->g, c->b, c->tolerance));
            advanceOut(n);
            break;
        }
        case NodeType::FindColor: {
            int zone = propInt(n, "zoneIndex", -1);
            if (zone >= 0) {
                bool found = host_.zonePresent ? host_.zonePresent(zone).value_or(false) : false;
                setOut(*cur_.g, n.id, "found", Value::makeBool(found));
                setOut(*cur_.g, n.id, "color", evalIn(n, "target"));
                setOut(*cur_.g, n.id, "similarity", Value::makeFloat(found ? 1.0 : 0.0));
                rt = {};
                advanceOut(n, found ? "found_exec" : "not_found_exec");
                break;
            }
            // Arbitrary region scan through the async probe sink.
            if (!host_.colorScan) {
                emitLog(1, "Find Color: no region-scan sink", n.id);
                rt = {};
                advanceOut(n, "not_found_exec");
                break;
            }
            {
                // Target set: the primary color + extras recorded on screen as
                // "r,g,b;r,g,b" — ANY matching color counts as found.
                ColorVal target = evalIn(n, "target").color;
                target.tolerance = propFloat(n, "tolerance", target.tolerance);
                std::vector<ColorVal> targets{target};
                {
                    std::string list = propStr(n, "colors", "");
                    size_t pos = 0;
                    while (pos <= list.size() && targets.size() < 12) {
                        const size_t semi = list.find(';', pos);
                        const std::string tok =
                            list.substr(pos, semi == std::string::npos ? semi : semi - pos);
                        pos = semi == std::string::npos ? list.size() + 1 : semi + 1;
                        if (tok.empty()) continue;
                        unsigned rr = 0, gg = 0, bb = 0;
                        if (std::sscanf(tok.c_str(), "%u,%u,%u", &rr, &gg, &bb) == 3)
                            targets.push_back(ColorVal{uint8_t(rr), uint8_t(gg), uint8_t(bb), 255,
                                                       target.tolerance});
                    }
                }
                // Region: a wired 'region' pin wins, then Custom props, else
                // the full frame.
                Rect region = rectFromObject(evalIn(n, "region"));
                bool regionOverride = false;
                if (const Port* rp = n.findInputByKey("region"))
                    for (const GraphConnection& c : cur_.g->connections)
                        if (c.toPort == rp->id && c.kind == PortKind::Data) regionOverride = true;
                if (!regionOverride && propInt(n, "regionMode", 0) == 2)
                    region = Rect{propFloat(n, "regionX"), propFloat(n, "regionY"),
                                  propFloat(n, "regionW", 1.0f), propFloat(n, "regionH", 1.0f)};
                // Scan colors sequentially: rt.phase = index, rt.auxF = best
                // similarity seen, rt.aux = index of that best, rt.from = its center.
                const int idx = std::clamp(rt.phase, 0, static_cast<int>(targets.size()) - 1);
                auto r = host_.colorScan(region, targets[idx]);
                if (!r) { state_ = RunState::Waiting; return; }
                if (r->similarity > rt.auxF) {
                    rt.auxF = r->similarity;
                    rt.aux = idx;
                    rt.from = r->center;
                }
                if (r->found) {
                    setOut(*cur_.g, n.id, "found", Value::makeBool(true));
                    setOut(*cur_.g, n.id, "coordinates", Value::makeCoord(r->center));
                    setOut(*cur_.g, n.id, "color",
                           Value::makeColor(r->color.r, r->color.g, r->color.b));
                    setOut(*cur_.g, n.id, "similarity", Value::makeFloat(r->similarity));
                    rt = {};
                    advanceOut(n, "found_exec");
                    break;
                }
                if (idx + 1 < static_cast<int>(targets.size())) {
                    rt.phase = idx + 1;
                    return;  // probe the next color next tick
                }
                setOut(*cur_.g, n.id, "found", Value::makeBool(false));
                setOut(*cur_.g, n.id, "coordinates", Value::makeCoord(rt.from));
                setOut(*cur_.g, n.id, "similarity", Value::makeFloat(rt.auxF));
                rt = {};
                advanceOut(n, "not_found_exec");
                break;
            }
        }
        case NodeType::FindObject: {
            if (!host_.objectScan) {
                emitLog(1, "Find Object: no detection sink (model not loaded?)", n.id);
                rt = {};
                advanceOut(n, "not_found_exec");
                break;
            }
            const int64_t timeoutMs = propInt(n, "timeoutMs", 0);
            if (rt.phase == 0) {
                rt.phase = 1;
                rt.untilUs = timeoutMs > 0 ? now + timeoutMs * 1000 : 0;
            }
            Rect region = rectFromObject(evalIn(n, "region"));
            bool regionOverride = false;
            if (const Port* rp = n.findInputByKey("region"))
                for (const GraphConnection& c : cur_.g->connections)
                    if (c.toPort == rp->id && c.kind == PortKind::Data) regionOverride = true;
            if (!regionOverride && propInt(n, "regionMode", 0) == 2)
                region = Rect{propFloat(n, "regionX"), propFloat(n, "regionY"),
                              propFloat(n, "regionW", 1.0f), propFloat(n, "regionH", 1.0f)};
            // Class filter: comma-separated names; empty = all classes.
            std::vector<std::string> classFilter;
            {
                std::string list = propStr(n, "classes", "");
                size_t pos = 0;
                while (pos <= list.size()) {
                    const size_t comma = list.find(',', pos);
                    std::string tok =
                        list.substr(pos, comma == std::string::npos ? comma : comma - pos);
                    pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
                    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
                    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
                    if (tok.empty()) continue;
                    classFilter.push_back(tok);
                }
            }
            const float conf = std::clamp(propInt(n, "confidencePct", 50), 1, 99) / 100.0f;
            auto r = host_.objectScan(region, classFilter, conf);
            if (!r) { state_ = RunState::Waiting; return; }
            if (r->found) {
                setOut(*cur_.g, n.id, "found", Value::makeBool(true));
                setOut(*cur_.g, n.id, "coordinates", Value::makeCoord(r->center));
                setOut(*cur_.g, n.id, "confidence", Value::makeFloat(r->confidence));
                setOut(*cur_.g, n.id, "className", Value::makeString(r->className));
                setOut(*cur_.g, n.id, "count", Value::makeInt(r->matchCount));
                std::map<std::string, Value> box;
                box["x"] = Value::makeFloat(r->bestBox.x); box["y"] = Value::makeFloat(r->bestBox.y);
                box["w"] = Value::makeFloat(r->bestBox.w); box["h"] = Value::makeFloat(r->bestBox.h);
                setOut(*cur_.g, n.id, "box", Value::makeObject(std::move(box)));
                rt = {};
                advanceOut(n, "found_exec");
                break;
            }
            rt.auxF = r->confidence;  // best sub-threshold score for diagnostics
            const bool timeout = rt.untilUs > 0 && now >= rt.untilUs;
            if (timeoutMs <= 0 || timeout) {
                setOut(*cur_.g, n.id, "found", Value::makeBool(false));
                setOut(*cur_.g, n.id, "confidence", Value::makeFloat(rt.auxF));
                rt = {};
                advanceOut(n, "not_found_exec");
                break;
            }
            state_ = RunState::Waiting;
            return;
        }
        case NodeType::ColorFound:
        case NodeType::ColorNotFound: {
            int zone = propInt(n, "zoneIndex", -1);
            bool matched = evalIn(n, "matched").asBool();
            if (zone >= 0 && host_.zonePresent) matched = host_.zonePresent(zone).value_or(false);
            bool yes = n.type == NodeType::ColorFound ? matched : !matched;
            advanceOut(n, yes ? "yes" : "no");
            break;
        }

        // ---------------- variables ----------------
        case NodeType::CreateVariable: {
            const std::string name = propStr(n, "name", "counter");
            Value v = evalIn(n, "initial");
            vars_[name] = v;
            setOut(*cur_.g, n.id, "value", v);
            advanceOut(n);
            break;
        }
        case NodeType::SetVariable: {
            const std::string name = propStr(n, "name", "counter");
            Value v = evalIn(n, "value");
            vars_[name] = v;
            setOut(*cur_.g, n.id, "out_value", v);
            advanceOut(n);
            break;
        }

        // ---------------- NEXPLAY commands ----------------
        case NodeType::RunMacro: {
            int idx = propInt(n, "macroIndex", -1);
            if (idx >= 0 && host_.runMacro) host_.runMacro(idx);
            else emitLog(1, "Run Macro: invalid macro index", n.id);
            advanceOut(n);
            break;
        }
        case NodeType::CheckColorZone: {
            int zone = propInt(n, "zoneIndex", 0);
            bool present = false;
            if (host_.zonePresent && zone >= 0 && zone < (int)automation_.zones.size())
                present = host_.zonePresent(zone).value_or(false);
            if (propBool(n, "negate", false)) present = !present;
            setOut(*cur_.g, n.id, "present", Value::makeBool(present));
            advanceOut(n, present ? "present_exec" : "absent_exec");
            break;
        }
        case NodeType::StopExecution:
            finish(RunState::Completed, propStr(n, "reason", "Stopped by node"));
            return;
        case NodeType::JumpTarget:
        case NodeType::CommentNode:
            advanceOut(n);
            break;

        case NodeType::CustomPlugin:
        default:
            emitLog(1, "unsupported node type — skipping", n.id);
            advanceOut(n);
            break;
    }
}

}  // namespace wasd::studio
