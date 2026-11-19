#include "core/studio_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace wasd::studio {
namespace fs = std::filesystem;
using nlohmann::json;

// ---------------------------------------------------------------- Value & Types

const char* valueTypeName(ValueType t) {
    switch (t) {
        case ValueType::Boolean: return "Boolean";
        case ValueType::Integer: return "Integer";
        case ValueType::Float: return "Float";
        case ValueType::String: return "String";
        case ValueType::Coordinate: return "Coordinate";
        case ValueType::Vector2: return "Vector2";
        case ValueType::Color: return "Color";
        case ValueType::Image: return "Image";
        case ValueType::ImageResult: return "ImageResult";
        case ValueType::Array: return "Array";
        case ValueType::Object: return "Object";
        case ValueType::Any: return "Any";
    }
    return "Boolean";
}

std::optional<ValueType> parseValueType(const std::string& s) {
    for (ValueType t : {ValueType::Boolean, ValueType::Integer, ValueType::Float, ValueType::String,
                        ValueType::Coordinate, ValueType::Vector2, ValueType::Color, ValueType::Image,
                        ValueType::ImageResult, ValueType::Array, ValueType::Object, ValueType::Any}) {
        if (s == valueTypeName(t)) return t;
    }
    return std::nullopt;
}

Value Value::makeBool(bool v) {
    Value out;
    out.type = ValueType::Boolean;
    out.b = v;
    return out;
}

Value Value::makeInt(int64_t v) {
    Value out;
    out.type = ValueType::Integer;
    out.i = v;
    return out;
}

Value Value::makeFloat(double v) {
    Value out;
    out.type = ValueType::Float;
    out.f = v;
    return out;
}

Value Value::makeString(std::string v) {
    Value out;
    out.type = ValueType::String;
    out.s = std::move(v);
    return out;
}

Value Value::makeCoord(Vec2 c) {
    Value out;
    out.type = ValueType::Coordinate;
    out.v = {std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f)};
    return out;
}

Value Value::makeVec2(Vec2 v) {
    Value out;
    out.type = ValueType::Vector2;
    out.v = v;
    return out;
}

Value Value::makeColor(uint8_t r, uint8_t g, uint8_t b, float tolerance) {
    Value out;
    out.type = ValueType::Color;
    out.color = {r, g, b, 255, tolerance};
    return out;
}

Value Value::makeImage(int refIndex, std::string label) {
    Value out;
    out.type = ValueType::Image;
    out.image.refIndex = refIndex;
    out.image.label = std::move(label);
    return out;
}

Value Value::makeImageResult(const ImageResultVal& r) {
    Value out;
    out.type = ValueType::ImageResult;
    out.imageResult = r;
    return out;
}

Value Value::makeArray(std::vector<Value> items) {
    Value out;
    out.type = ValueType::Array;
    out.arr = std::move(items);
    return out;
}

Value Value::makeObject(std::map<std::string, Value> fields) {
    Value out;
    out.type = ValueType::Object;
    out.obj = std::move(fields);
    return out;
}

Value Value::defaultFor(ValueType t) {
    switch (t) {
        case ValueType::Boolean: return makeBool(false);
        case ValueType::Integer: return makeInt(0);
        case ValueType::Float: return makeFloat(0.0);
        case ValueType::String: return makeString({});
        case ValueType::Coordinate: return makeCoord({0.5f, 0.5f});
        case ValueType::Vector2: return makeVec2({0.0f, 0.0f});
        case ValueType::Color: return makeColor(255, 0, 0, 12.0f);
        case ValueType::Image: return makeImage(-1);
        case ValueType::ImageResult: return makeImageResult({});
        case ValueType::Array: return makeArray({});
        case ValueType::Object: return makeObject({});
        case ValueType::Any: return makeString({});
    }
    return makeBool(false);
}

bool Value::asBool() const {
    switch (type) {
        case ValueType::Boolean: return b;
        case ValueType::Integer: return i != 0;
        case ValueType::Float: return std::abs(f) > 1e-9;
        case ValueType::String: return !s.empty() && s != "false" && s != "0";
        case ValueType::ImageResult: return imageResult.found;
        case ValueType::Image: return image.refIndex >= 0 || image.captureSlot >= 0;
        case ValueType::Array: return !arr.empty();
        case ValueType::Object: return !obj.empty();
        default: return false;
    }
}

int64_t Value::asInt() const {
    switch (type) {
        case ValueType::Integer: return i;
        case ValueType::Float: return static_cast<int64_t>(std::llround(f));
        case ValueType::Boolean: return b ? 1 : 0;
        case ValueType::String: {
            try {
                return s.empty() ? 0 : std::stoll(s);
            } catch (...) {
                return 0;
            }
        }
        default: return 0;
    }
}

double Value::asFloat() const {
    switch (type) {
        case ValueType::Float: return f;
        case ValueType::Integer: return static_cast<double>(i);
        case ValueType::Boolean: return b ? 1.0 : 0.0;
        case ValueType::ImageResult: return imageResult.similarity;
        case ValueType::String: {
            try {
                return s.empty() ? 0.0 : std::stod(s);
            } catch (...) {
                return 0.0;
            }
        }
        default: return 0.0;
    }
}

std::string Value::asString() const {
    char buf[96];
    switch (type) {
        case ValueType::String: return s;
        case ValueType::Boolean: return b ? "true" : "false";
        case ValueType::Integer: return std::to_string(i);
        case ValueType::Float:
            std::snprintf(buf, sizeof buf, "%.4g", f);
            return buf;
        case ValueType::Coordinate:
        case ValueType::Vector2:
            std::snprintf(buf, sizeof buf, "(%.3f, %.3f)", v.x, v.y);
            return buf;
        case ValueType::Color:
            std::snprintf(buf, sizeof buf, "#%02X%02X%02X", color.r, color.g, color.b);
            return buf;
        case ValueType::Image:
            return image.label.empty() ? ("Image#" + std::to_string(image.refIndex)) : image.label;
        case ValueType::ImageResult:
            std::snprintf(buf, sizeof buf, "%s (%.1f%%)", imageResult.found ? "Found" : "NotFound",
                          imageResult.similarity * 100.0f);
            return buf;
        case ValueType::Array:
            return "[" + std::to_string(arr.size()) + " items]";
        case ValueType::Object:
            return "{" + std::to_string(obj.size()) + " keys}";
        case ValueType::Any:
            return s;
    }
    return {};
}

Vec2 Value::asCoord() const {
    if (type == ValueType::Coordinate || type == ValueType::Vector2)
        return {std::clamp(v.x, 0.0f, 1.0f), std::clamp(v.y, 0.0f, 1.0f)};
    if (type == ValueType::ImageResult) return imageResult.coordinates;
    return {0.5f, 0.5f};
}

Vec2 Value::asVec2() const {
    if (type == ValueType::Vector2 || type == ValueType::Coordinate) return v;
    if (type == ValueType::ImageResult) return imageResult.coordinates;
    return {0.0f, 0.0f};
}

bool Value::isEmpty() const {
    switch (type) {
        case ValueType::String: return s.empty();
        case ValueType::Array: return arr.empty();
        case ValueType::Object: return obj.empty();
        case ValueType::Image: return image.refIndex < 0 && image.captureSlot < 0;
        case ValueType::ImageResult: return !imageResult.found;
        default: return false;
    }
}

bool areDataTypesCompatible(ValueType fromOut, ValueType toIn) {
    if (fromOut == toIn) return true;
    if (fromOut == ValueType::Any || toIn == ValueType::Any) return true;
    // Safe lossless / standard numeric & geometric promotions:
    if (fromOut == ValueType::Integer && toIn == ValueType::Float) return true;
    if (fromOut == ValueType::Coordinate && toIn == ValueType::Vector2) return true;
    if (fromOut == ValueType::Vector2 && toIn == ValueType::Coordinate) return true;
    return false;
}

Value coerceValue(const Value& v, ValueType targetType) {
    if (targetType == ValueType::Any || v.type == targetType) return v;
    switch (targetType) {
        case ValueType::Boolean: return Value::makeBool(v.asBool());
        case ValueType::Integer: return Value::makeInt(v.asInt());
        case ValueType::Float: return Value::makeFloat(v.asFloat());
        case ValueType::String: return Value::makeString(v.asString());
        case ValueType::Coordinate: return Value::makeCoord(v.asCoord());
        case ValueType::Vector2: return Value::makeVec2(v.asVec2());
        default: return Value::defaultFor(targetType);
    }
}

// ---------------------------------------------------------------- Node Catalog

const char* categoryName(NodeCategory c) {
    switch (c) {
        case NodeCategory::FlowControl: return "Flow Control";
        case NodeCategory::InputActions: return "Input Actions";
        case NodeCategory::ImageRecognition: return "Image Recognition";
        case NodeCategory::ColorRecognition: return "Color Recognition";
        case NodeCategory::Logic: return "Logic";
        case NodeCategory::VariablesMath: return "Variables & Math";
        case NodeCategory::NexplayCommands: return "NEXPLAY Commands";
    }
    return "Flow Control";
}

namespace {

PortSpec execIn(const char* key = "in", const char* name = "In") {
    return {key, name, PortKind::Execution, ValueType::Boolean, PortDir::Input, {}};
}
PortSpec execOut(const char* key = "out", const char* name = "Out") {
    return {key, name, PortKind::Execution, ValueType::Boolean, PortDir::Output, {}};
}
PortSpec dataIn(const char* key, const char* name, ValueType t, Value def) {
    return {key, name, PortKind::Data, t, PortDir::Input, std::move(def)};
}
PortSpec dataOut(const char* key, const char* name, ValueType t) {
    return {key, name, PortKind::Data, t, PortDir::Output, Value::defaultFor(t)};
}

std::vector<NodeTypeSpec> buildBuiltInSpecs() {
    std::vector<NodeTypeSpec> S;
    S.reserve(64);

    // ==================== A. Flow Control ====================
    S.push_back({NodeType::Start, "flow.start", "Start", NodeCategory::FlowControl, "\uE768",
                 "Entry point where graph execution begins.",
                 "Connect Start -> Find Image or Delay to begin your flow.", false,
                 {execOut("out", "Exec")},
                 {{"runMode", "Run Mode", Value::makeInt(0), false, 0, 3, "0=Once, 1=Continuous"}}});

    S.push_back({NodeType::End, "flow.end", "End", NodeCategory::FlowControl, "\uE71A",
                 "Cleanly finishes the current execution path.",
                 "Connect the final step of a branch to End.", false,
                 {execIn("in", "Exec")},
                 {{"statusText", "Completion Note", Value::makeString("Completed"), true, 0, 0, ""}}});

    S.push_back({NodeType::Delay, "flow.delay", "Delay", NodeCategory::FlowControl, "\uE823",
                 "Pauses execution for a configurable duration without blocking the UI.",
                 "Start -> Delay (250 ms) -> Click", false,
                 {execIn(), execOut(),
                  dataIn("duration", "Duration", ValueType::Integer, Value::makeInt(250))},
                 {{"duration", "Delay Duration", Value::makeInt(250), false, 0, 600000, "Base wait duration"},
                  {"unit", "Time Unit", Value::makeInt(0), false, 0, 1, "0 = Milliseconds, 1 = Seconds"},
                  {"randomJitterMs", "Random +/- Jitter (ms)", Value::makeInt(0), true, 0, 60000, "Optional randomized timing"}}});

    S.push_back({NodeType::Loop, "flow.loop", "Loop", NodeCategory::FlowControl, "\uE8EE",
                 "Repeats the Loop Body a fixed number of times or infinitely until Break.",
                 "Loop (Count=5) -> Body -> Click", false,
                 {execIn(),
                  dataIn("count", "Count", ValueType::Integer, Value::makeInt(3)),
                  execOut("body", "Loop Body"),
                  execOut("completed", "Completed"),
                  dataOut("index", "Index", ValueType::Integer)},
                 {{"count", "Loop Count", Value::makeInt(3), false, 1, 10000, "Iterations when Infinite is off"},
                  {"infinite", "Infinite Loop", Value::makeBool(false), false, 0, 1, "Repeat until Break or Stop"},
                  {"pauseMs", "Pause Between Iterations (ms)", Value::makeInt(0), true, 0, 60000, ""}}});

    S.push_back({NodeType::ForLoop, "flow.for_loop", "For Loop", NodeCategory::FlowControl, "\uE8EE",
                 "Iterates an index from First Index to Last Index with configurable Step.",
                 "For Loop (0..9) -> Index drives logic", false,
                 {execIn(),
                  dataIn("first", "First Index", ValueType::Integer, Value::makeInt(0)),
                  dataIn("last", "Last Index", ValueType::Integer, Value::makeInt(4)),
                  dataIn("step", "Step", ValueType::Integer, Value::makeInt(1)),
                  execOut("body", "Loop Body"),
                  execOut("completed", "Completed"),
                  dataOut("index", "Index", ValueType::Integer)},
                 {{"first", "First Index", Value::makeInt(0), false, -100000, 100000, ""},
                  {"last", "Last Index", Value::makeInt(4), false, -100000, 100000, ""},
                  {"step", "Step", Value::makeInt(1), true, -10000, 10000, "Non-zero step"}}});

    S.push_back({NodeType::While, "flow.while", "While", NodeCategory::FlowControl, "\uE8EE",
                 "Repeats Loop Body while Condition evaluates to true (bounded by safety limit).",
                 "Connect Image Found -> Condition", false,
                 {execIn(),
                  dataIn("condition", "Condition", ValueType::Boolean, Value::makeBool(false)),
                  execOut("body", "Loop Body"),
                  execOut("completed", "Completed"),
                  dataOut("iteration", "Iteration", ValueType::Integer)},
                 {{"maxIterations", "Max Iterations Guard", Value::makeInt(1000), true, 1, 100000, "Prevents runaway loops"},
                  {"pollDelayMs", "Iteration Delay (ms)", Value::makeInt(16), true, 0, 60000, ""}}});

    S.push_back({NodeType::Break, "flow.break", "Break", NodeCategory::FlowControl, "\uE71A",
                 "Exits the innermost active Loop / For Loop / While immediately.",
                 "If Condition -> Break", false,
                 {execIn()}, {}});

    S.push_back({NodeType::Continue, "flow.continue", "Continue", NodeCategory::FlowControl, "\uE72A",
                 "Skips the rest of the current loop iteration and starts the next iteration.",
                 "If SkipCondition -> Continue", false,
                 {execIn()}, {}});

    S.push_back({NodeType::If, "flow.if", "If", NodeCategory::FlowControl, "\uE8FB",
                 "Branches execution to True or False (Else) based on a Boolean condition.",
                 "Find Image (Found) -> If -> True / False", false,
                 {execIn(),
                  dataIn("condition", "Condition", ValueType::Boolean, Value::makeBool(true)),
                  execOut("true", "True"),
                  execOut("false", "False")},
                 {{"negate", "Invert Condition", Value::makeBool(false), true, 0, 1, ""}}});

    S.push_back({NodeType::Else, "flow.else", "Else", NodeCategory::FlowControl, "\uE8FB",
                 "Explicit visual continuation node for False / fallback execution paths.",
                 "If (False) -> Else -> Fallback Action", false,
                 {execIn("in", "From False"), execOut("out", "Out")}, {}});

    S.push_back({NodeType::Switch, "flow.switch", "Switch", NodeCategory::FlowControl, "\uE8AB",
                 "Routes execution to Case 0, Case 1, Case 2 or Default based on Value.",
                 "Switch on Integer state (0, 1, 2)", false,
                 {execIn(),
                  dataIn("value", "Selection", ValueType::Integer, Value::makeInt(0)),
                  execOut("case0", "Case 0"),
                  execOut("case1", "Case 1"),
                  execOut("case2", "Case 2"),
                  execOut("default", "Default")},
                 {{"case0Val", "Case 0 Value", Value::makeInt(0), false, -100000, 100000, ""},
                  {"case1Val", "Case 1 Value", Value::makeInt(1), false, -100000, 100000, ""},
                  {"case2Val", "Case 2 Value", Value::makeInt(2), false, -100000, 100000, ""}}});

    S.push_back({NodeType::Sequence, "flow.sequence", "Sequence", NodeCategory::FlowControl, "\uE8FD",
                 "Executes Then 0, Then 1, and Then 2 in strict order.",
                 "Organize multi-step flows cleanly without long chains.", false,
                 {execIn(),
                  execOut("then0", "Then 0"),
                  execOut("then1", "Then 1"),
                  execOut("then2", "Then 2")}, {}});

    S.push_back({NodeType::Function, "flow.function", "Function", NodeCategory::FlowControl, "\uE943",
                 "Invokes a reusable Function subgraph defined inside this studio project.",
                 "Encapsulate repeated routines into a clean callable node.", false,
                 {execIn(),
                  dataIn("arg", "Argument", ValueType::Any, Value::makeInt(0)),
                  execOut(),
                  dataOut("return", "Return", ValueType::Any)},
                 {{"functionName", "Function Name", Value::makeString("MyFunction"), false, 0, 0, ""}}});

    S.push_back({NodeType::Subgraph, "flow.subgraph", "Subgraph", NodeCategory::FlowControl, "\uE8B2",
                 "Executes an embedded Subgraph and resumes when it reaches End.",
                 "Group complex detection stages into a modular subgraph.", false,
                 {execIn(), execOut()},
                 {{"subgraphId", "Subgraph ID", Value::makeString(""), false, 0, 0, ""}}});

    // ==================== B. Input Actions ====================
    S.push_back({NodeType::Click, "input.click", "Click", NodeCategory::InputActions, "\uE7C9",
                 "Taps a normalized screen coordinate and cleanly releases the virtual finger.",
                 "Find Image (Coordinates) -> Click", false,
                 {execIn(), execOut(),
                  dataIn("point", "Position", ValueType::Coordinate, Value::makeCoord({0.5f, 0.5f}))},
                 {{"point", "Screen Point (X, Y)", Value::makeCoord({0.5f, 0.5f}), false, 0.0, 1.0, ""},
                  {"holdMs", "Tap Dwell (ms)", Value::makeInt(40), true, 15, 1000, ""},
                  {"finger", "Virtual Finger (1-5)", Value::makeInt(0), true, 0, 4, ""}}});

    S.push_back({NodeType::DoubleClick, "input.double_click", "Double Click", NodeCategory::InputActions, "\uE7C9",
                 "Performs two crisp taps at a normalized screen coordinate.",
                 "Click item twice to equip or confirm.", false,
                 {execIn(), execOut(),
                  dataIn("point", "Position", ValueType::Coordinate, Value::makeCoord({0.5f, 0.5f}))},
                 {{"point", "Screen Point (X, Y)", Value::makeCoord({0.5f, 0.5f}), false, 0.0, 1.0, ""},
                  {"gapMs", "Gap Between Taps (ms)", Value::makeInt(75), true, 20, 500, ""},
                  {"finger", "Virtual Finger (1-5)", Value::makeInt(0), true, 0, 4, ""}}});

    S.push_back({NodeType::LongPress, "input.long_press", "Long Press", NodeCategory::InputActions, "\uE7C9",
                 "Holds a touch finger at the target coordinate for Duration ms, then releases.",
                 "Long Press (800 ms) on inventory slot.", false,
                 {execIn(), execOut(),
                  dataIn("point", "Position", ValueType::Coordinate, Value::makeCoord({0.5f, 0.5f})),
                  dataIn("duration", "Duration (ms)", ValueType::Integer, Value::makeInt(500))},
                 {{"point", "Screen Point (X, Y)", Value::makeCoord({0.5f, 0.5f}), false, 0.0, 1.0, ""},
                  {"duration", "Hold Duration (ms)", Value::makeInt(500), false, 40, 600000, ""},
                  {"finger", "Virtual Finger (1-5)", Value::makeInt(0), true, 0, 4, ""}}});

    S.push_back({NodeType::PressDown, "input.press_down", "Press Down", NodeCategory::InputActions, "\uE7C9",
                 "Places or moves a virtual finger without lifting (auto-released on stop/cancel).",
                 "Press Down -> Delay -> Release", false,
                 {execIn(), execOut(),
                  dataIn("point", "Position", ValueType::Coordinate, Value::makeCoord({0.5f, 0.5f}))},
                 {{"point", "Screen Point (X, Y)", Value::makeCoord({0.5f, 0.5f}), false, 0.0, 1.0, ""},
                  {"finger", "Virtual Finger (1-5)", Value::makeInt(0), false, 0, 4, ""}}});

    S.push_back({NodeType::Release, "input.release", "Release", NodeCategory::InputActions, "\uE7C9",
                 "Lifts a previously held virtual finger cleanly.",
                 "Pair with Press Down.", false,
                 {execIn(), execOut()},
                 {{"finger", "Virtual Finger (1-5)", Value::makeInt(0), false, 0, 4, ""}}});

    S.push_back({NodeType::Slide, "input.slide", "Slide", NodeCategory::InputActions, "\uE72A",
                 "Smoothly drags a virtual finger from Start Point to End Point over Duration ms.",
                 "Scroll a list or swipe a dialog.", false,
                 {execIn(), execOut(),
                  dataIn("from", "Start", ValueType::Coordinate, Value::makeCoord({0.5f, 0.7f})),
                  dataIn("to", "End", ValueType::Coordinate, Value::makeCoord({0.5f, 0.3f})),
                  dataIn("duration", "Duration (ms)", ValueType::Integer, Value::makeInt(220))},
                 {{"from", "Start Point (X, Y)", Value::makeCoord({0.5f, 0.7f}), false, 0.0, 1.0, ""},
                  {"to", "End Point (X, Y)", Value::makeCoord({0.5f, 0.3f}), false, 0.0, 1.0, ""},
                  {"duration", "Slide Duration (ms)", Value::makeInt(220), false, 16, 60000, ""},
                  {"finger", "Virtual Finger (1-5)", Value::makeInt(0), true, 0, 4, ""}}});

    S.push_back({NodeType::KeyPress, "input.key_press", "Key Press", NodeCategory::InputActions, "\uE765",
                 "Triggers a mapped virtual key (Tap/Press or Hold Down; auto-released on cancel).",
                 "Trigger Space or reload binding.", false,
                 {execIn(), execOut()},
                 {{"key", "Virtual Key Code", Value::makeInt(0x20), false, 0, 254, ""},
                  {"holdOnly", "Keep Held (until Key Release)", Value::makeBool(false), true, 0, 1, ""},
                  {"holdMs", "Tap Hold Duration (ms)", Value::makeInt(45), true, 15, 60000, ""}}});

    S.push_back({NodeType::KeyRelease, "input.key_release", "Key Release", NodeCategory::InputActions, "\uE765",
                 "Releases a virtual key previously held by Key Press.",
                 "Pair with Key Press (Keep Held = on).", false,
                 {execIn(), execOut()},
                 {{"key", "Virtual Key Code", Value::makeInt(0x20), false, 0, 254, ""}}});

    S.push_back({NodeType::Wait, "input.wait", "Wait", NodeCategory::InputActions, "\uE823",
                 "Waits for the specified milliseconds before advancing to the next action.",
                 "Click -> Wait (150 ms) -> Click", false,
                 {execIn(), execOut(),
                  dataIn("ms", "Milliseconds", ValueType::Integer, Value::makeInt(100))},
                 {{"ms", "Wait Duration (ms)", Value::makeInt(100), false, 0, 600000, ""}}});

    // ==================== C. Image Recognition ====================
    S.push_back({NodeType::CaptureScreenshot, "image.capture", "Capture Screenshot", NodeCategory::ImageRecognition, "\uE722",
                 "Snapshots the current phone frame (or region) for downstream inspection.",
                 "Capture Screenshot -> Find Image", false,
                 {execIn(), execOut(),
                  dataIn("region", "Region", ValueType::Object, Value::makeObject({})),
                  dataOut("image", "Image", ValueType::Image)},
                 {{"fullScreen", "Full Screen", Value::makeBool(true), false, 0, 1, ""},
                  {"regionX", "Region X", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionY", "Region Y", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionW", "Region W", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"regionH", "Region H", Value::makeFloat(1.0), true, 0.01, 1.0, ""}}});

    S.push_back({NodeType::FindImage, "image.find", "Find Image", NodeCategory::ImageRecognition, "\uEB9F",
                 "Searches for a reference image asynchronously and branches on Found / Not Found.",
                 "Find Image -> Found -> Click (Coordinates)", false,
                 {execIn(),
                  dataIn("image", "Ref Image", ValueType::Image, Value::makeImage(-1)),
                  dataIn("region", "Search Region", ValueType::Object, Value::makeObject({})),
                  execOut("found_exec", "Found"),
                  execOut("not_found_exec", "Not Found"),
                  dataOut("found", "Found", ValueType::Boolean),
                  dataOut("similarity", "Similarity", ValueType::Float),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("duration", "Search Duration", ValueType::Float),
                  dataOut("out_region", "Search Region", ValueType::Object),
                  dataOut("result", "Result", ValueType::ImageResult)},
                 {{"imageIndex", "Reference Image", Value::makeInt(0), false, -1, 64, ""},
                  {"thresholdPct", "Similarity Threshold (%)", Value::makeInt(85), false, 1, 100, "0 = use image default"},
                  {"regionMode", "Region Mode", Value::makeInt(0), false, 0, 2, "0=Saved, 1=FullScreen, 2=Custom"},
                  {"regionX", "Custom Region X", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionY", "Custom Region Y", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionW", "Custom Region W", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"regionH", "Custom Region H", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"intervalMs", "Search Interval (ms)", Value::makeInt(120), true, 16, 10000, ""},
                  {"maxAttempts", "Maximum Attempts", Value::makeInt(1), true, 0, 1000, "0 = until timeout"},
                  {"continuous", "Continuous Search", Value::makeBool(false), true, 0, 1, ""},
                  {"timeoutMs", "Search Timeout (ms)", Value::makeInt(0), false, 0, 600000, "0 = single check"},
                  {"matchPolicy", "Match Policy", Value::makeInt(0), true, 0, 3, "0=Best, 1=First, 2=Nearest, 3=All"}}});

    S.push_back({NodeType::ImageFound, "image.is_found", "Image Found", NodeCategory::ImageRecognition, "\uE73E",
                 "Routes execution only when an ImageResult indicates a positive match.",
                 "Result -> Image Found -> Action", false,
                 {execIn(),
                  dataIn("result", "Image Result", ValueType::ImageResult, Value::makeImageResult({})),
                  execOut("yes", "Found"),
                  execOut("no", "Otherwise"),
                  dataOut("found", "Is Found", ValueType::Boolean)}, {}});

    S.push_back({NodeType::ImageNotFound, "image.not_found", "Image Not Found", NodeCategory::ImageRecognition, "\uE711",
                 "Routes execution when an ImageResult indicates the target was absent.",
                 "Result -> Image Not Found -> Fallback", false,
                 {execIn(),
                  dataIn("result", "Image Result", ValueType::ImageResult, Value::makeImageResult({})),
                  execOut("yes", "Not Found"),
                  execOut("no", "Otherwise"),
                  dataOut("not_found", "Is Missing", ValueType::Boolean)}, {}});

    S.push_back({NodeType::CompareSimilarity, "image.compare_sim", "Compare Similarity", NodeCategory::ImageRecognition, "\uE9D9",
                 "Checks whether an ImageResult or similarity score meets a minimum threshold.",
                 "Similarity >= 0.90 -> Boolean", true,
                 {dataIn("similarity", "Similarity", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("threshold", "Min Similarity", ValueType::Float, Value::makeFloat(0.85)),
                  dataOut("meets", "Meets Threshold", ValueType::Boolean),
                  dataOut("delta", "Margin", ValueType::Float)},
                 {{"threshold", "Threshold (0..1)", Value::makeFloat(0.85), false, 0.0, 1.0, ""}}});

    S.push_back({NodeType::GetImageCoordinates, "image.get_coords", "Get Image Coordinates", NodeCategory::ImageRecognition, "\uE81D",
                 "Extracts click coordinates (with optional relative offset) from an ImageResult.",
                 "Find Image (Result) -> Get Image Coordinates -> Click", true,
                 {dataIn("result", "Image Result", ValueType::ImageResult, Value::makeImageResult({})),
                  dataIn("offset", "Box Offset", ValueType::Vector2, Value::makeVec2({0.5f, 0.5f})),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("found", "Found", ValueType::Boolean)},
                 {{"offsetX", "Offset X (0..1)", Value::makeFloat(0.5), false, 0.0, 1.0, ""},
                  {"offsetY", "Offset Y (0..1)", Value::makeFloat(0.5), false, 0.0, 1.0, ""}}});

    S.push_back({NodeType::SelectSearchRegion, "image.select_region", "Select Search Region", NodeCategory::ImageRecognition, "\uE740",
                 "Outputs a normalized rectangular Search Region object for image or color queries.",
                 "Select Search Region -> Find Image (Search Region)", true,
                 {dataOut("region", "Search Region", ValueType::Object)},
                 {{"x", "Left (X)", Value::makeFloat(0.0), false, 0.0, 1.0, ""},
                  {"y", "Top (Y)", Value::makeFloat(0.0), false, 0.0, 1.0, ""},
                  {"w", "Width (W)", Value::makeFloat(1.0), false, 0.01, 1.0, ""},
                  {"h", "Height (H)", Value::makeFloat(1.0), false, 0.01, 1.0, ""}}});

    S.push_back({NodeType::WaitForImage, "image.wait_for", "Wait For Image", NodeCategory::ImageRecognition, "\uE823",
                 "Polls asynchronously until the reference image appears or Timeout expires.",
                 "Wait For Image (5000 ms) -> Found -> Click", false,
                 {execIn(),
                  execOut("found_exec", "Found"),
                  execOut("timeout_exec", "Timed Out"),
                  dataOut("found", "Found", ValueType::Boolean),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("similarity", "Similarity", ValueType::Float),
                  dataOut("result", "Result", ValueType::ImageResult)},
                 {{"imageIndex", "Reference Image", Value::makeInt(0), false, -1, 64, ""},
                  {"timeoutMs", "Timeout (ms)", Value::makeInt(5000), false, 50, 600000, ""},
                  {"intervalMs", "Poll Interval (ms)", Value::makeInt(120), true, 16, 10000, ""},
                  {"thresholdPct", "Similarity Threshold (%)", Value::makeInt(85), true, 1, 100, ""}}});

    S.push_back({NodeType::ImageSearchResult, "image.unpack_result", "Image Search Result", NodeCategory::ImageRecognition, "\uE8A5",
                 "Unpacks an ImageResult value into all individual data pins.",
                 "Result -> Found, Similarity, Coordinates, Duration", true,
                 {dataIn("result", "Result", ValueType::ImageResult, Value::makeImageResult({})),
                  dataOut("found", "Found", ValueType::Boolean),
                  dataOut("similarity", "Similarity", ValueType::Float),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("duration", "Duration (ms)", ValueType::Float),
                  dataOut("region", "Search Region", ValueType::Object)}, {}});

    S.push_back({NodeType::FindObject, "object.find", "Find Object", NodeCategory::ImageRecognition, "\uE8B5",
                 "Detects objects (enemies, soldiers, weapons) with a YOLO ONNX model in a region.",
                 "Find Object -> Found -> Click (Coordinates)", false,
                 {execIn(),
                  dataIn("region", "Search Region", ValueType::Object, Value::makeObject({})),
                  execOut("found_exec", "Found"),
                  execOut("not_found_exec", "Not Found"),
                  dataOut("found", "Found", ValueType::Boolean),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("confidence", "Confidence", ValueType::Float),
                  dataOut("className", "Class Name", ValueType::String),
                  dataOut("count", "Count", ValueType::Integer),
                  dataOut("box", "Bounding Box", ValueType::Object)},
                 {{"classes", "Class Filter", Value::makeString("soldier,camouflage_soldier"), false, 0, 0,
                   "Comma-separated class names; empty = all classes"},
                  {"confidencePct", "Min Confidence (%)", Value::makeInt(50), false, 1, 99, ""},
                  {"regionMode", "Region Mode", Value::makeInt(0), false, 0, 2, "0=Full Screen, 2=Custom"},
                  {"regionX", "Custom Region X", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionY", "Custom Region Y", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionW", "Custom Region W", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"regionH", "Custom Region H", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"timeoutMs", "Search Timeout (ms)", Value::makeInt(0), false, 0, 600000, "0 = single check"}}});

    // ==================== D. Color Recognition ====================
    S.push_back({NodeType::GetPixelColor, "color.get_pixel", "Get Pixel Color", NodeCategory::ColorRecognition, "\uEF3C",
                 "Reads the live color at a normalized screen coordinate.",
                 "Get Pixel Color -> Compare Color", false,
                 {execIn(), execOut(),
                  dataIn("point", "Position", ValueType::Coordinate, Value::makeCoord({0.5f, 0.5f})),
                  dataOut("color", "Color", ValueType::Color)},
                 {{"point", "Sample Point (X, Y)", Value::makeCoord({0.5f, 0.5f}), false, 0.0, 1.0, ""}}});

    S.push_back({NodeType::FindColor, "color.find", "Find Color", NodeCategory::ColorRecognition, "\uEF3C",
                 "Scans a Color Zone or screen region for a target color using CIELAB Delta-E.",
                 "Find Color -> Found -> Click (Coordinates)", false,
                 {execIn(),
                  dataIn("target", "Target Color", ValueType::Color, Value::makeColor(255, 58, 69, 12.0f)),
                  dataIn("region", "Search Region", ValueType::Object, Value::makeObject({})),
                  execOut("found_exec", "Found"),
                  execOut("not_found_exec", "Not Found"),
                  dataOut("found", "Found", ValueType::Boolean),
                  dataOut("coordinates", "Coordinates", ValueType::Coordinate),
                  dataOut("color", "Matched Color", ValueType::Color),
                  dataOut("similarity", "Similarity", ValueType::Float)},
                 {{"targetColor", "Target Color", Value::makeColor(255, 58, 69, 12.0f), false, 0, 255, ""},
                  {"colors", "Extra Target Colors", Value::makeString(""), false, 0, 0,
                   "Recorded on screen as r,g,b;r,g,b — a match on ANY color counts"},
                  {"regionMode", "Region Mode", Value::makeInt(0), false, 0, 2, "0=Full Screen, 2=Custom"},
                  {"tolerance", "CIELAB Tolerance", Value::makeFloat(12.0), true, 1.0, 100.0, ""},
                  {"zoneIndex", "Saved Color Zone (-1 = custom)", Value::makeInt(-1), true, -1, 64, ""},
                  {"regionX", "Custom Region X", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionY", "Custom Region Y", Value::makeFloat(0.0), true, 0.0, 1.0, ""},
                  {"regionW", "Custom Region W", Value::makeFloat(1.0), true, 0.01, 1.0, ""},
                  {"regionH", "Custom Region H", Value::makeFloat(1.0), true, 0.01, 1.0, ""}}});

    S.push_back({NodeType::CompareColor, "color.compare", "Compare Color", NodeCategory::ColorRecognition, "\uEF3C",
                 "Compares two Colors in perceptual CIELAB space and outputs match & similarity.",
                 "Get Pixel Color -> Compare Color -> If", true,
                 {dataIn("a", "Sampled Color", ValueType::Color, Value::makeColor(255, 0, 0)),
                  dataIn("b", "Reference Color", ValueType::Color, Value::makeColor(255, 0, 0)),
                  dataOut("matched", "Matched", ValueType::Boolean),
                  dataOut("similarity", "Similarity", ValueType::Float),
                  dataOut("deltaE", "Delta-E Distance", ValueType::Float)},
                 {{"tolerance", "Max Delta-E", Value::makeFloat(12.0), false, 0.5, 100.0, ""}}});

    S.push_back({NodeType::ColorFound, "color.is_found", "Color Found", NodeCategory::ColorRecognition, "\uE73E",
                 "Branches execution when a Color Zone or color test is positive.",
                 "Color Found -> Action", false,
                 {execIn(),
                  dataIn("matched", "Matched", ValueType::Boolean, Value::makeBool(false)),
                  execOut("yes", "Found"),
                  execOut("no", "Otherwise")},
                 {{"zoneIndex", "Color Zone (-1 = use pin)", Value::makeInt(-1), false, -1, 64, ""}}});

    S.push_back({NodeType::ColorNotFound, "color.not_found", "Color Not Found", NodeCategory::ColorRecognition, "\uE711",
                 "Branches execution when the target color is absent.",
                 "Color Not Found -> Retry / Fallback", false,
                 {execIn(),
                  dataIn("matched", "Matched", ValueType::Boolean, Value::makeBool(false)),
                  execOut("yes", "Not Found"),
                  execOut("no", "Otherwise")},
                 {{"zoneIndex", "Color Zone (-1 = use pin)", Value::makeInt(-1), false, -1, 64, ""}}});

    S.push_back({NodeType::ColorSimilarity, "color.similarity", "Color Similarity", NodeCategory::ColorRecognition, "\uE9D9",
                 "Computes normalized perceptual similarity [0..1] between two colors.",
                 "Color Similarity -> Greater Than (0.9)", true,
                 {dataIn("a", "Color A", ValueType::Color, Value::makeColor(255, 0, 0)),
                  dataIn("b", "Color B", ValueType::Color, Value::makeColor(255, 0, 0)),
                  dataOut("similarity", "Similarity", ValueType::Float)}, {}});

    // ==================== E. Logic ====================
    S.push_back({NodeType::LogicAnd, "logic.and", "AND", NodeCategory::Logic, "\uE945",
                 "True only when both A and B are true.", "Combine two detection conditions.", true,
                 {dataIn("a", "A", ValueType::Boolean, Value::makeBool(false)),
                  dataIn("b", "B", ValueType::Boolean, Value::makeBool(false)),
                  dataOut("result", "Result", ValueType::Boolean)}, {}});

    S.push_back({NodeType::LogicOr, "logic.or", "OR", NodeCategory::Logic, "\uE945",
                 "True when either A or B is true.", "Fallback detection trigger.", true,
                 {dataIn("a", "A", ValueType::Boolean, Value::makeBool(false)),
                  dataIn("b", "B", ValueType::Boolean, Value::makeBool(false)),
                  dataOut("result", "Result", ValueType::Boolean)}, {}});

    S.push_back({NodeType::LogicNot, "logic.not", "NOT", NodeCategory::Logic, "\uE945",
                 "Inverts a Boolean value (true <-> false).", "Found -> NOT -> Is Missing", true,
                 {dataIn("value", "Value", ValueType::Boolean, Value::makeBool(false)),
                  dataOut("result", "Result", ValueType::Boolean)}, {}});

    S.push_back({NodeType::Equal, "logic.equal", "Equal", NodeCategory::Logic, "\uE94E",
                 "Returns true when A equals B.", "Counter == 5 -> Stop", true,
                 {dataIn("a", "A", ValueType::Any, Value::makeInt(0)),
                  dataIn("b", "B", ValueType::Any, Value::makeInt(0)),
                  dataOut("result", "Equal", ValueType::Boolean)}, {}});

    S.push_back({NodeType::NotEqual, "logic.not_equal", "Not Equal", NodeCategory::Logic, "\uE94E",
                 "Returns true when A and B differ.", "State != 0 -> Act", true,
                 {dataIn("a", "A", ValueType::Any, Value::makeInt(0)),
                  dataIn("b", "B", ValueType::Any, Value::makeInt(0)),
                  dataOut("result", "Not Equal", ValueType::Boolean)}, {}});

    S.push_back({NodeType::GreaterThan, "logic.greater", "Greater Than", NodeCategory::Logic, "\uE96E",
                 "Returns true when numeric A > B.", "Similarity > 0.85", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(0.0)),
                  dataOut("result", "A > B", ValueType::Boolean)}, {}});

    S.push_back({NodeType::LessThan, "logic.less", "Less Than", NodeCategory::Logic, "\uE96F",
                 "Returns true when numeric A < B.", "Attempts < 3", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(0.0)),
                  dataOut("result", "A < B", ValueType::Boolean)}, {}});

    S.push_back({NodeType::IsEmpty, "logic.is_empty", "Is Empty", NodeCategory::Logic, "\uE8C6",
                 "Checks if a String, Array, Object, Image or ImageResult is empty / unset.",
                 "Check whether a result has data.", true,
                 {dataIn("value", "Value", ValueType::Any, Value::makeString({})),
                  dataOut("empty", "Is Empty", ValueType::Boolean)}, {}});

    // ==================== F. Variables & Math ====================
    S.push_back({NodeType::CreateVariable, "var.create", "Create Variable", NodeCategory::VariablesMath, "\uE8F1",
                 "Initializes or resets a named graph variable during execution.",
                 "Create Variable 'attempts' = 0", false,
                 {execIn(), execOut(),
                  dataIn("initial", "Initial Value", ValueType::Any, Value::makeInt(0)),
                  dataOut("value", "Value", ValueType::Any)},
                 {{"name", "Variable Name", Value::makeString("counter"), false, 0, 0, ""},
                  {"varType", "Variable Type", Value::makeInt(static_cast<int>(ValueType::Integer)), false, 0, 10, ""}}});

    S.push_back({NodeType::SetVariable, "var.set", "Set Variable", NodeCategory::VariablesMath, "\uE8F1",
                 "Writes a new value into a reusable graph variable.",
                 "Add (counter, 1) -> Set Variable 'counter'", false,
                 {execIn(), execOut(),
                  dataIn("value", "Value", ValueType::Any, Value::makeInt(0)),
                  dataOut("out_value", "Value", ValueType::Any)},
                 {{"name", "Variable Name", Value::makeString("counter"), false, 0, 0, ""}}});

    S.push_back({NodeType::GetVariable, "var.get", "Get Variable", NodeCategory::VariablesMath, "\uE8F1",
                 "Reads the current value of a named graph variable.",
                 "Get Variable 'counter' -> Less Than (5)", true,
                 {dataOut("value", "Value", ValueType::Any)},
                 {{"name", "Variable Name", Value::makeString("counter"), false, 0, 0, ""}}});

    S.push_back({NodeType::MathAdd, "math.add", "Add", NodeCategory::VariablesMath, "\uE710",
                 "Computes A + B.", "Increment counters or offsets.", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(1.0)),
                  dataOut("result", "Result", ValueType::Float)}, {}});

    S.push_back({NodeType::MathSubtract, "math.sub", "Subtract", NodeCategory::VariablesMath, "\uE738",
                 "Computes A - B.", "Remaining retries = Max - Current", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(1.0)),
                  dataOut("result", "Result", ValueType::Float)}, {}});

    S.push_back({NodeType::MathMultiply, "math.mul", "Multiply", NodeCategory::VariablesMath, "\uE947",
                 "Computes A * B.", "Scale delay or factor.", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(1.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(2.0)),
                  dataOut("result", "Result", ValueType::Float)}, {}});

    S.push_back({NodeType::MathDivide, "math.div", "Divide", NodeCategory::VariablesMath, "\uE94A",
                 "Computes A / B (protected against division by zero).", "Normalize values safely.", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(1.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(2.0)),
                  dataOut("result", "Result", ValueType::Float)}, {}});

    S.push_back({NodeType::MathClamp, "math.clamp", "Clamp", NodeCategory::VariablesMath, "\uE8EB",
                 "Constrains Value to stay within [Min, Max].", "Clamp delay to [50, 1000] ms", true,
                 {dataIn("value", "Value", ValueType::Float, Value::makeFloat(0.5)),
                  dataIn("min", "Min", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("max", "Max", ValueType::Float, Value::makeFloat(1.0)),
                  dataOut("result", "Clamped", ValueType::Float)}, {}});

    S.push_back({NodeType::MathMin, "math.min", "Min", NodeCategory::VariablesMath, "\uE74B",
                 "Returns the smaller of A and B.", "", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(1.0)),
                  dataOut("result", "Min", ValueType::Float)}, {}});

    S.push_back({NodeType::MathMax, "math.max", "Max", NodeCategory::VariablesMath, "\uE74A",
                 "Returns the larger of A and B.", "", true,
                 {dataIn("a", "A", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("b", "B", ValueType::Float, Value::makeFloat(1.0)),
                  dataOut("result", "Max", ValueType::Float)}, {}});

    S.push_back({NodeType::RandomValue, "math.random", "Random Value", NodeCategory::VariablesMath, "\uE8B1",
                 "Generates a random float (and integer) within [Min, Max].",
                 "Randomize delay between 120 and 180 ms.", true,
                 {dataIn("min", "Min", ValueType::Float, Value::makeFloat(0.0)),
                  dataIn("max", "Max", ValueType::Float, Value::makeFloat(100.0)),
                  dataOut("float_val", "Float", ValueType::Float),
                  dataOut("int_val", "Integer", ValueType::Integer)},
                 {{"min", "Min", Value::makeFloat(0.0), false, -100000.0, 100000.0, ""},
                  {"max", "Max", Value::makeFloat(100.0), false, -100000.0, 100000.0, ""}}});

    // ==================== G. Existing NEXPLAY Commands ====================
    S.push_back({NodeType::RunMacro, "nexplay.run_macro", "Run Macro", NodeCategory::NexplayCommands, "\uE945",
                 "Executes an existing Simple-Mode Macro from the active profile.",
                 "Reuse legacy macro steps directly inside a visual graph.", false,
                 {execIn(), execOut()},
                 {{"macroIndex", "Target Macro", Value::makeInt(0), false, -1, 256, ""},
                  {"macroName", "Macro Name Hint", Value::makeString(""), true, 0, 0, ""}}});

    S.push_back({NodeType::CheckColorZone, "nexplay.color_zone", "Check Color Zone", NodeCategory::NexplayCommands, "\uEF3C",
                 "Queries one of NEXPLAY's circular Color Zones (Any/All multi-color CIELAB match).",
                 "Check Color Zone -> Present -> Action", false,
                 {execIn(),
                  execOut("present_exec", "Present"),
                  execOut("absent_exec", "Absent"),
                  dataOut("present", "Present", ValueType::Boolean)},
                 {{"zoneIndex", "Color Zone", Value::makeInt(0), false, 0, 64, ""},
                  {"negate", "Invert Result", Value::makeBool(false), true, 0, 1, ""}}});

    S.push_back({NodeType::StopExecution, "nexplay.stop", "Stop Execution", NodeCategory::NexplayCommands, "\uE71A",
                 "Stops the entire running graph immediately and releases all synthetic touches/keys.",
                 "Emergency or terminal stop condition.", false,
                 {execIn()},
                 {{"reason", "Stop Message", Value::makeString("Stopped by node"), true, 0, 0, ""}}});

    S.push_back({NodeType::JumpTarget, "nexplay.jump_target", "Jump Target", NodeCategory::NexplayCommands, "\uE71B",
                 "Named routing anchor used by converted legacy JumpStep macros.",
                 "Preserves arbitrary step jumps losslessly.", false,
                 {execIn(), execOut()},
                 {{"label", "Anchor Label", Value::makeString("Step_1"), false, 0, 0, ""}}});

    S.push_back({NodeType::CommentNode, "nexplay.comment", "Comment Step", NodeCategory::NexplayCommands, "\uE90A",
                 "Pass-through documentation step preserved from Simple Mode comments.",
                 "Annotate a specific stage in the execution chain.", false,
                 {execIn(), execOut()},
                 {{"text", "Comment Text", Value::makeString(""), false, 0, 0, ""}}});

    return S;
}

std::vector<NodeTypeSpec>& mutableSpecs() {
    static std::vector<NodeTypeSpec> specs = buildBuiltInSpecs();
    return specs;
}

}  // namespace

const std::vector<NodeTypeSpec>& allNodeSpecs() { return mutableSpecs(); }

const NodeTypeSpec* findNodeSpec(NodeType t, const std::string& customKey) {
    const auto& all = allNodeSpecs();
    if (t == NodeType::CustomPlugin && !customKey.empty()) {
        for (const auto& s : all)
            if (s.type == NodeType::CustomPlugin && s.typeKey == customKey) return &s;
        return nullptr;
    }
    for (const auto& s : all)
        if (s.type == t) return &s;
    return nullptr;
}

void registerCustomNodeSpec(NodeTypeSpec spec) {
    spec.type = NodeType::CustomPlugin;
    auto& all = mutableSpecs();
    for (auto& existing : all) {
        if (existing.typeKey == spec.typeKey) {
            existing = std::move(spec);
            return;
        }
    }
    all.push_back(std::move(spec));
}

const char* nodeTypeKey(NodeType t) {
    if (const auto* s = findNodeSpec(t)) return s->typeKey.c_str();
    return "custom.plugin";
}

std::optional<NodeType> parseNodeTypeKey(const std::string& key) {
    for (const auto& s : allNodeSpecs())
        if (s.typeKey == key) return s.type;
    return std::nullopt;
}

// ---------------------------------------------------------------- GraphNode

const Port* GraphNode::findPort(uint32_t portId) const {
    for (const auto& p : inputs)
        if (p.id == portId) return &p;
    for (const auto& p : outputs)
        if (p.id == portId) return &p;
    return nullptr;
}

Port* GraphNode::findPort(uint32_t portId) {
    for (auto& p : inputs)
        if (p.id == portId) return &p;
    for (auto& p : outputs)
        if (p.id == portId) return &p;
    return nullptr;
}

const Port* GraphNode::findInputByKey(const std::string& key) const {
    for (const auto& p : inputs)
        if (p.key == key) return &p;
    return nullptr;
}

const Port* GraphNode::findOutputByKey(const std::string& key) const {
    for (const auto& p : outputs)
        if (p.key == key) return &p;
    return nullptr;
}

Value GraphNode::getProp(const std::string& key, const Value& fallback) const {
    const auto it = props.find(key);
    return it != props.end() ? it->second : fallback;
}

void GraphNode::setProp(const std::string& key, Value v) { props[key] = std::move(v); }

// ---------------------------------------------------------------- AutomationGraph

GraphNode* AutomationGraph::findNode(uint32_t nodeId) {
    for (auto& n : nodes)
        if (n.id == nodeId) return &n;
    return nullptr;
}

const GraphNode* AutomationGraph::findNode(uint32_t nodeId) const {
    for (const auto& n : nodes)
        if (n.id == nodeId) return &n;
    return nullptr;
}

bool AutomationGraph::findPortOwner(uint32_t portId, const GraphNode*& outNode, const Port*& outPort) const {
    for (const auto& n : nodes) {
        if (const Port* p = n.findPort(portId)) {
            outNode = &n;
            outPort = p;
            return true;
        }
    }
    outNode = nullptr;
    outPort = nullptr;
    return false;
}

GraphNode& AutomationGraph::addNode(NodeType type, Vec2 pos, const std::string& customKey) {
    GraphNode n;
    n.id = allocateId();
    n.type = type;
    n.customTypeKey = customKey;
    n.pos = pos;
    if (const NodeTypeSpec* spec = findNodeSpec(type, customKey)) {
        n.title = spec->displayName;
        for (const PortSpec& ps : spec->ports) {
            Port p;
            p.id = allocateId();
            p.key = ps.key;
            p.name = ps.name;
            p.kind = ps.kind;
            p.dataType = ps.dataType;
            p.dir = ps.dir;
            p.defaultValue = ps.defaultValue;
            if (p.dir == PortDir::Input) n.inputs.push_back(std::move(p));
            else n.outputs.push_back(std::move(p));
        }
        for (const PropertySpec& pr : spec->properties) n.props[pr.key] = pr.defaultValue;
    }
    nodes.push_back(std::move(n));
    return nodes.back();
}

bool AutomationGraph::removeNode(uint32_t nodeId) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) { return n.id == nodeId; });
    if (it == nodes.end()) return false;
    connections.erase(std::remove_if(connections.begin(), connections.end(),
                                     [&](const GraphConnection& c) { return c.fromNode == nodeId || c.toNode == nodeId; }),
                      connections.end());
    for (auto& g : groups)
        g.nodeIds.erase(std::remove(g.nodeIds.begin(), g.nodeIds.end(), nodeId), g.nodeIds.end());
    nodes.erase(it);
    return true;
}

ConnectionCheck AutomationGraph::canConnect(uint32_t nodeA, uint32_t portA, uint32_t nodeB, uint32_t portB) const {
    ConnectionCheck res;
    const GraphNode* na = findNode(nodeA);
    const GraphNode* nb = findNode(nodeB);
    if (!na || !nb) {
        res.code = RejectReason::NodeNotFound;
        res.reason = "Target node does not exist";
        return res;
    }
    const Port* pa = na->findPort(portA);
    const Port* pb = nb->findPort(portB);
    if (!pa || !pb) {
        res.code = RejectReason::PortNotFound;
        res.reason = "Target port does not exist";
        return res;
    }
    if (nodeA == nodeB) {
        res.code = RejectReason::SelfConnection;
        res.reason = "Cannot connect a node to itself";
        return res;
    }
    if (pa->dir == pb->dir) {
        res.code = RejectReason::SameDirection;
        res.reason = pa->dir == PortDir::Input ? "Cannot connect two Input ports together"
                                               : "Cannot connect two Output ports together";
        return res;
    }

    // Normalize to Output -> Input regardless of drag direction.
    const GraphNode* outN = pa->dir == PortDir::Output ? na : nb;
    const Port* outP      = pa->dir == PortDir::Output ? pa : pb;
    const GraphNode* inN  = pa->dir == PortDir::Input ? na : nb;
    const Port* inP       = pa->dir == PortDir::Input ? pa : pb;

    if (outP->kind != inP->kind) {
        res.code = RejectReason::KindMismatch;
        res.reason = "Cannot connect an Execution port to a Data port";
        return res;
    }

    if (outP->kind == PortKind::Data && !areDataTypesCompatible(outP->dataType, inP->dataType)) {
        res.code = RejectReason::TypeMismatch;
        res.reason = std::string("Incompatible data types: ") + valueTypeName(outP->dataType) +
                     " cannot feed " + valueTypeName(inP->dataType);
        return res;
    }

    for (const auto& c : connections) {
        if (c.fromNode == outN->id && c.fromPort == outP->id && c.toNode == inN->id && c.toPort == inP->id) {
            res.code = RejectReason::DuplicateWire;
            res.reason = "These ports are already connected";
            return res;
        }
    }

    // Prevent pure data dependency cycles (Execution loops ARE valid for While / retry flows,
    // whereas a Data wire cycle A.out -> B.in -> A.in has no topological evaluation order).
    if (outP->kind == PortKind::Data) {
        std::vector<uint32_t> stack{inN->id};
        std::unordered_set<uint32_t> visited;
        while (!stack.empty()) {
            const uint32_t cur = stack.back();
            stack.pop_back();
            if (cur == outN->id) {
                res.code = RejectReason::DataCycle;
                res.reason = "Data connection would create a circular dependency";
                return res;
            }
            if (!visited.insert(cur).second) continue;
            for (const auto& c : connections)
                if (c.kind == PortKind::Data && c.fromNode == cur) stack.push_back(c.toNode);
        }
    }

    res.ok = true;
    res.code = RejectReason::None;
    res.outNode = outN->id;
    res.outPort = outP->id;
    res.inNode = inN->id;
    res.inPort = inP->id;
    res.kind = outP->kind;
    return res;
}

ConnectionCheck AutomationGraph::connect(uint32_t nodeA, uint32_t portA, uint32_t nodeB, uint32_t portB,
                                         uint32_t* outConnId, bool replaceExisting) {
    ConnectionCheck chk = canConnect(nodeA, portA, nodeB, portB);
    if (!chk.ok) return chk;

    if (replaceExisting) {
        // Execution Output is single-target (deterministic next node); Data Input is single-source.
        if (chk.kind == PortKind::Execution) {
            connections.erase(std::remove_if(connections.begin(), connections.end(),
                                             [&](const GraphConnection& c) {
                                                 return c.kind == PortKind::Execution && c.fromPort == chk.outPort;
                                             }),
                              connections.end());
        } else {
            connections.erase(std::remove_if(connections.begin(), connections.end(),
                                             [&](const GraphConnection& c) {
                                                 return c.kind == PortKind::Data && c.toPort == chk.inPort;
                                             }),
                              connections.end());
        }
    }

    GraphConnection conn;
    conn.id = allocateId();
    conn.fromNode = chk.outNode;
    conn.fromPort = chk.outPort;
    conn.toNode = chk.inNode;
    conn.toPort = chk.inPort;
    conn.kind = chk.kind;
    if (outConnId) *outConnId = conn.id;
    connections.push_back(conn);
    return chk;
}

ConnectionCheck AutomationGraph::connectByKey(uint32_t outNodeId, const std::string& outPortKey,
                                              uint32_t inNodeId, const std::string& inPortKey,
                                              uint32_t* outConnId, bool replaceExisting) {
    const GraphNode* fn = findNode(outNodeId);
    const GraphNode* tn = findNode(inNodeId);
    if (!fn || !tn) return {false, RejectReason::NodeNotFound, "Target node does not exist"};
    const Port* fp = fn->findOutputByKey(outPortKey);
    const Port* tp = tn->findInputByKey(inPortKey);
    if (!fp || !tp) return {false, RejectReason::PortNotFound, "Target port key does not exist"};
    return connect(outNodeId, fp->id, inNodeId, tp->id, outConnId, replaceExisting);
}

bool AutomationGraph::disconnect(uint32_t connectionId) {
    const auto before = connections.size();
    connections.erase(std::remove_if(connections.begin(), connections.end(),
                                     [&](const GraphConnection& c) { return c.id == connectionId; }),
                      connections.end());
    return connections.size() != before;
}

int AutomationGraph::disconnectPort(uint32_t portId) {
    const auto before = connections.size();
    connections.erase(std::remove_if(connections.begin(), connections.end(),
                                     [&](const GraphConnection& c) { return c.fromPort == portId || c.toPort == portId; }),
                      connections.end());
    return static_cast<int>(before - connections.size());
}

GraphClipboard AutomationGraph::copySelection(const std::vector<uint32_t>& nodeIds,
                                              const std::vector<uint32_t>& noteIds) const {
    GraphClipboard clip;
    std::unordered_set<uint32_t> picked(nodeIds.begin(), nodeIds.end());
    for (const auto& n : nodes)
        if (picked.count(n.id)) clip.nodes.push_back(n);
    for (const auto& c : connections)
        if (picked.count(c.fromNode) && picked.count(c.toNode)) clip.internalConnections.push_back(c);
    std::unordered_set<uint32_t> pickedNotes(noteIds.begin(), noteIds.end());
    for (const auto& nt : notes)
        if (pickedNotes.count(nt.id)) clip.notes.push_back(nt);
    return clip;
}

std::vector<uint32_t> AutomationGraph::pasteClipboard(const GraphClipboard& clip, Vec2 offset) {
    std::vector<uint32_t> newIds;
    if (clip.empty()) return newIds;

    std::unordered_map<uint32_t, uint32_t> nodeMap;
    std::unordered_map<uint32_t, uint32_t> portMap;

    for (GraphNode copy : clip.nodes) {
        const uint32_t oldNodeId = copy.id;
        copy.id = allocateId();
        copy.pos = {copy.pos.x + offset.x, copy.pos.y + offset.y};
        nodeMap[oldNodeId] = copy.id;
        for (Port& p : copy.inputs) {
            const uint32_t oldP = p.id;
            p.id = allocateId();
            portMap[oldP] = p.id;
        }
        for (Port& p : copy.outputs) {
            const uint32_t oldP = p.id;
            p.id = allocateId();
            portMap[oldP] = p.id;
        }
        newIds.push_back(copy.id);
        nodes.push_back(std::move(copy));
    }

    for (const GraphConnection& c : clip.internalConnections) {
        auto fn = nodeMap.find(c.fromNode), tn = nodeMap.find(c.toNode);
        auto fp = portMap.find(c.fromPort), tp = portMap.find(c.toPort);
        if (fn != nodeMap.end() && tn != nodeMap.end() && fp != portMap.end() && tp != portMap.end()) {
            GraphConnection nc;
            nc.id = allocateId();
            nc.fromNode = fn->second;
            nc.fromPort = fp->second;
            nc.toNode = tn->second;
            nc.toPort = tp->second;
            nc.kind = c.kind;
            connections.push_back(nc);
        }
    }

    for (GraphNote nt : clip.notes) {
        nt.id = allocateId();
        nt.pos = {nt.pos.x + offset.x, nt.pos.y + offset.y};
        notes.push_back(std::move(nt));
    }
    return newIds;
}

GraphGroup& AutomationGraph::addGroup(const std::string& title, Rect rect, const std::vector<uint32_t>& memberNodes) {
    GraphGroup g;
    g.id = allocateId();
    g.title = title;
    g.rect = rect;
    g.nodeIds = memberNodes;
    groups.push_back(std::move(g));
    return groups.back();
}

bool AutomationGraph::removeGroup(uint32_t groupId) {
    const auto before = groups.size();
    groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const GraphGroup& g) { return g.id == groupId; }),
                 groups.end());
    return groups.size() != before;
}

GraphNote& AutomationGraph::addNote(const std::string& title, const std::string& text, Vec2 pos) {
    GraphNote n;
    n.id = allocateId();
    n.title = title;
    n.text = text;
    n.pos = pos;
    notes.push_back(std::move(n));
    return notes.back();
}

bool AutomationGraph::removeNote(uint32_t noteId) {
    const auto before = notes.size();
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const GraphNote& n) { return n.id == noteId; }),
                notes.end());
    return notes.size() != before;
}

bool AutomationGraph::upsertVariable(GraphVariable var) {
    if (var.name.empty()) return false;
    for (auto& existing : variables) {
        if (existing.name == var.name) {
            existing = std::move(var);
            return true;
        }
    }
    variables.push_back(std::move(var));
    return true;
}

bool AutomationGraph::removeVariable(const std::string& varName) {
    const auto before = variables.size();
    variables.erase(std::remove_if(variables.begin(), variables.end(),
                                   [&](const GraphVariable& v) { return v.name == varName; }),
                    variables.end());
    return variables.size() != before;
}

const GraphVariable* AutomationGraph::findVariable(const std::string& varName) const {
    for (const auto& v : variables)
        if (v.name == varName) return &v;
    return nullptr;
}

void AutomationGraph::autoArrange(float startX, float startY, float colSpacing, float rowSpacing) {
    if (nodes.empty()) return;
    std::unordered_map<uint32_t, int> layer;
    std::queue<uint32_t> q;

    for (const auto& n : nodes) {
        layer[n.id] = 0;
        if (n.type == NodeType::Start) q.push(n.id);
    }
    if (q.empty()) {
        // Seed with nodes that have no incoming wires.
        std::unordered_set<uint32_t> hasIn;
        for (const auto& c : connections) hasIn.insert(c.toNode);
        for (const auto& n : nodes)
            if (!hasIn.count(n.id)) q.push(n.id);
    }

    // Bounded BFS layering so valid execution back-edges (loops) never spin forever.
    const int maxLayer = static_cast<int>(nodes.size()) + 2;
    int visits = 0;
    const int visitCap = static_cast<int>(nodes.size() * 8 + 16);
    while (!q.empty() && visits++ < visitCap) {
        const uint32_t u = q.front();
        q.pop();
        const int lu = layer[u];
        for (const auto& c : connections) {
            if (c.fromNode == u && lu + 1 < maxLayer && layer[c.toNode] < lu + 1) {
                layer[c.toNode] = lu + 1;
                q.push(c.toNode);
            }
        }
    }

    std::map<int, std::vector<GraphNode*>> columns;
    for (auto& n : nodes) columns[layer[n.id]].push_back(&n);

    int colIdx = 0;
    for (auto& [lvl, list] : columns) {
        (void)lvl;
        for (std::size_t r = 0; r < list.size(); ++r) {
            list[r]->pos = {startX + colIdx * colSpacing, startY + static_cast<float>(r) * rowSpacing};
        }
        ++colIdx;
    }
}

// ---------------------------------------------------------------- GraphHistory

void GraphHistory::reset(const AutomationGraph& g) {
    base_ = g;
    undo_.clear();
    redo_.clear();
}

bool GraphHistory::commit(const AutomationGraph& g) {
    if (g == base_) return false;
    undo_.push_back(std::move(base_));
    if (undo_.size() > limit_) undo_.erase(undo_.begin());
    base_ = g;
    redo_.clear();
    return true;
}

bool GraphHistory::undo(AutomationGraph& out) {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(base_));
    base_ = std::move(undo_.back());
    undo_.pop_back();
    out = base_;
    return true;
}

bool GraphHistory::redo(AutomationGraph& out) {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(base_));
    base_ = std::move(redo_.back());
    redo_.pop_back();
    out = base_;
    return true;
}

// ---------------------------------------------------------------- Factories & Templates

std::string generateGraphId() {
    static std::mt19937_64 rng{std::random_device{}()};
    const uint64_t a = rng(), b = rng();
    char buf[40];
    std::snprintf(buf, sizeof buf, "nxg_%08llx%08llx",
                  static_cast<unsigned long long>(a & 0xFFFFFFFFull),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFull));
    return buf;
}

AutomationGraph makeStarterGraph(const std::string& name) {
    AutomationGraph g;
    g.id = generateGraphId();
    g.name = name;
    g.addNode(NodeType::Start, {80.0f, 180.0f});
    return g;
}

AutomationGraph makeTemplateGraph(GraphTemplateKind kind) {
    AutomationGraph g;
    g.id = generateGraphId();
    g.nodes.reserve(8);

    switch (kind) {
        case GraphTemplateKind::BasicSequence: {
            g.name = "Basic Sequence";
            g.metadata.description = "Start -> Delay -> Click -> End";
            const uint32_t st = g.addNode(NodeType::Start, {80.0f, 180.0f}).id;
            const uint32_t dl = g.addNode(NodeType::Delay, {340.0f, 180.0f}).id;
            const uint32_t cl = g.addNode(NodeType::Click, {620.0f, 180.0f}).id;
            const uint32_t en = g.addNode(NodeType::End, {900.0f, 180.0f}).id;
            g.connectByKey(st, "out", dl, "in");
            g.connectByKey(dl, "out", cl, "in");
            g.connectByKey(cl, "out", en, "in");
            break;
        }
        case GraphTemplateKind::ConditionalFlow: {
            g.name = "Conditional Flow";
            g.metadata.description = "Check Color Zone -> If True Click, Else Wait";
            const uint32_t st = g.addNode(NodeType::Start, {60.0f, 200.0f}).id;
            const uint32_t cz = g.addNode(NodeType::CheckColorZone, {300.0f, 200.0f}).id;
            const uint32_t iff = g.addNode(NodeType::If, {580.0f, 200.0f}).id;
            const uint32_t cl = g.addNode(NodeType::Click, {860.0f, 120.0f}).id;
            const uint32_t wt = g.addNode(NodeType::Wait, {860.0f, 300.0f}).id;
            const uint32_t en = g.addNode(NodeType::End, {1140.0f, 200.0f}).id;
            g.connectByKey(st, "out", cz, "in");
            g.connectByKey(cz, "present_exec", iff, "in");
            g.connectByKey(cz, "present", iff, "condition");
            g.connectByKey(iff, "true", cl, "in");
            g.connectByKey(iff, "false", wt, "in");
            g.connectByKey(cl, "out", en, "in");
            g.connectByKey(wt, "out", en, "in");
            break;
        }
        case GraphTemplateKind::ImageRecognitionExample: {
            g.name = "Image Recognition Flow";
            g.metadata.description = "Find Image -> Found clicks detected coordinates; Not Found delays and retries";
            const uint32_t st = g.addNode(NodeType::Start, {60.0f, 200.0f}).id;
            const uint32_t fi = g.addNode(NodeType::FindImage, {300.0f, 180.0f}).id;
            const uint32_t cl = g.addNode(NodeType::Click, {640.0f, 120.0f}).id;
            const uint32_t dl = g.addNode(NodeType::Delay, {640.0f, 320.0f}).id;
            const uint32_t en = g.addNode(NodeType::End, {930.0f, 120.0f}).id;
            g.connectByKey(st, "out", fi, "in");
            g.connectByKey(fi, "found_exec", cl, "in");
            g.connectByKey(fi, "coordinates", cl, "point");
            g.connectByKey(cl, "out", en, "in");
            g.connectByKey(fi, "not_found_exec", dl, "in");
            g.connectByKey(dl, "out", en, "in");
            break;
        }
        case GraphTemplateKind::ColorDetectionExample: {
            g.name = "Color Detection Flow";
            g.metadata.description = "Find Color -> Click matched coordinates when detected";
            const uint32_t st = g.addNode(NodeType::Start, {60.0f, 200.0f}).id;
            const uint32_t fc = g.addNode(NodeType::FindColor, {310.0f, 180.0f}).id;
            const uint32_t cl = g.addNode(NodeType::Click, {620.0f, 160.0f}).id;
            const uint32_t en = g.addNode(NodeType::End, {900.0f, 160.0f}).id;
            g.connectByKey(st, "out", fc, "in");
            g.connectByKey(fc, "found_exec", cl, "in");
            g.connectByKey(fc, "coordinates", cl, "point");
            g.connectByKey(cl, "out", en, "in");
            break;
        }
        case GraphTemplateKind::LoopExample: {
            g.name = "Loop Example";
            g.metadata.description = "Repeats Click + Delay 5 times, then finishes";
            const uint32_t st = g.addNode(NodeType::Start, {60.0f, 200.0f}).id;
            GraphNode& lpNode = g.addNode(NodeType::Loop, {300.0f, 180.0f});
            lpNode.setProp("count", Value::makeInt(5));
            const uint32_t lp = lpNode.id;
            const uint32_t cl = g.addNode(NodeType::Click, {600.0f, 120.0f}).id;
            const uint32_t dl = g.addNode(NodeType::Delay, {880.0f, 120.0f}).id;
            const uint32_t en = g.addNode(NodeType::End, {600.0f, 310.0f}).id;
            g.connectByKey(st, "out", lp, "in");
            g.connectByKey(lp, "body", cl, "in");
            g.connectByKey(cl, "out", dl, "in");
            g.connectByKey(lp, "completed", en, "in");
            break;
        }
        case GraphTemplateKind::VariablesExample: {
            g.name = "Variables & Math Example";
            g.metadata.description = "Initializes 'taps' variable, increments it with Add, and stores it";
            g.upsertVariable({"taps", ValueType::Integer, Value::makeInt(0), "Tap counter"});
            const uint32_t st = g.addNode(NodeType::Start, {60.0f, 200.0f}).id;
            GraphNode& gvNode = g.addNode(NodeType::GetVariable, {300.0f, 340.0f});
            gvNode.setProp("name", Value::makeString("taps"));
            const uint32_t gv = gvNode.id;
            const uint32_t ad = g.addNode(NodeType::MathAdd, {540.0f, 340.0f}).id;
            GraphNode& svNode = g.addNode(NodeType::SetVariable, {780.0f, 200.0f});
            svNode.setProp("name", Value::makeString("taps"));
            const uint32_t sv = svNode.id;
            const uint32_t en = g.addNode(NodeType::End, {1060.0f, 200.0f}).id;
            g.connectByKey(st, "out", sv, "in");
            g.connectByKey(gv, "value", ad, "a");
            g.connectByKey(ad, "result", sv, "value");
            g.connectByKey(sv, "out", en, "in");
            break;
        }
    }
    return g;
}

// ---------------------------------------------------------------- Validation

std::vector<std::string> validateGraph(const AutomationGraph& g, int depth) {
    std::vector<std::string> errs;
    if (depth > kMaxGraphDepth) {
        errs.push_back("subgraph nesting exceeds maximum depth");
        return errs;
    }
    if (g.name.empty()) errs.push_back("graph name cannot be empty");
    if (static_cast<int>(g.nodes.size()) > kMaxGraphNodes)
        errs.push_back("graph exceeds maximum node count (" + std::to_string(kMaxGraphNodes) + ")");
    if (g.view.zoom < 0.1f || g.view.zoom > 4.0f)
        errs.push_back("canvas zoom out of supported range [0.1, 4.0]");
    if (g.metadata.maxDurationMs < 50 || g.metadata.maxDurationMs > 3600000)
        errs.push_back("graph maxDurationMs out of range");
    if (g.metadata.maxStepsBudget < 10 || g.metadata.maxStepsBudget > 1000000)
        errs.push_back("graph maxStepsBudget out of range");

    std::unordered_set<uint32_t> nodeIds;
    std::unordered_set<uint32_t> portIds;
    for (const auto& n : g.nodes) {
        if (n.id == 0 || !nodeIds.insert(n.id).second)
            errs.push_back("duplicate or zero node ID: " + std::to_string(n.id));
        for (const auto& p : n.inputs) {
            if (p.id == 0 || !portIds.insert(p.id).second)
                errs.push_back("duplicate or zero input port ID: " + std::to_string(p.id));
            if (p.dir != PortDir::Input) errs.push_back("port in inputs has Output direction");
        }
        for (const auto& p : n.outputs) {
            if (p.id == 0 || !portIds.insert(p.id).second)
                errs.push_back("duplicate or zero output port ID: " + std::to_string(p.id));
            if (p.dir != PortDir::Output) errs.push_back("port in outputs has Input direction");
        }
    }

    std::unordered_set<uint32_t> connIds;
    for (const auto& c : g.connections) {
        if (c.id == 0 || !connIds.insert(c.id).second)
            errs.push_back("duplicate or zero connection ID: " + std::to_string(c.id));
        const GraphNode* fn = g.findNode(c.fromNode);
        const GraphNode* tn = g.findNode(c.toNode);
        if (!fn || !tn) {
            errs.push_back("connection references a missing node");
            continue;
        }
        if (c.fromNode == c.toNode) errs.push_back("self-connection on node " + std::to_string(c.fromNode));
        const Port* fp = fn->findPort(c.fromPort);
        const Port* tp = tn->findPort(c.toPort);
        if (!fp || !tp) {
            errs.push_back("connection references a missing port");
            continue;
        }
        if (fp->dir != PortDir::Output || tp->dir != PortDir::Input)
            errs.push_back("connection direction must be Output -> Input");
        if (fp->kind != tp->kind || fp->kind != c.kind)
            errs.push_back("connection mixes Execution and Data ports");
        if (fp->kind == PortKind::Data && !areDataTypesCompatible(fp->dataType, tp->dataType))
            errs.push_back("incompatible data wire: " + std::string(valueTypeName(fp->dataType)) + " -> " +
                           valueTypeName(tp->dataType));
    }

    std::unordered_set<std::string> varNames;
    for (const auto& v : g.variables) {
        if (v.name.empty()) errs.push_back("variable with empty name");
        else if (!varNames.insert(v.name).second) errs.push_back("duplicate variable: " + v.name);
    }

    // Two Start entries must not share a hotkey — dispatch would only fire the
    // first match and the second flow would be unreachable.
    {
        std::map<int, int> keyUse;
        bool first = true;
        for (const auto& n : g.nodes) {
            if (n.type != NodeType::Start) continue;
            const int k1 = n.triggerKey ? n.triggerKey : (first ? g.metadata.triggerKey : 0);
            const int k2 = n.triggerKey2 ? n.triggerKey2 : (first ? g.metadata.triggerKey2 : 0);
            first = false;
            if (k1 > 0) ++keyUse[k1];
            if (k2 > 0) ++keyUse[k2];
        }
        for (const auto& [k, c] : keyUse)
            if (c > 1) errs.push_back("duplicate trigger key on Start nodes");
    }

    for (const auto& sub : g.subgraphs) {
        auto subErrs = validateGraph(sub, depth + 1);
        for (auto& e : subErrs) errs.push_back("subgraph '" + sub.name + "': " + e);
    }
    return errs;
}

// ---------------------------------------------------------------- JSON & Migration

namespace {

json valueToJson(const Value& v, int depth = 0) {
    if (depth > 8) return json{{"t", "Boolean"}, {"v", false}};
    json j{{"t", valueTypeName(v.type)}};
    switch (v.type) {
        case ValueType::Boolean: j["v"] = v.b; break;
        case ValueType::Integer: j["v"] = v.i; break;
        case ValueType::Float: j["v"] = v.f; break;
        case ValueType::String:
        case ValueType::Any: j["v"] = v.s; break;
        case ValueType::Coordinate:
        case ValueType::Vector2: j["v"] = json::array({v.v.x, v.v.y}); break;
        case ValueType::Color:
            j["v"] = {{"r", v.color.r}, {"g", v.color.g}, {"b", v.color.b},
                      {"a", v.color.a}, {"tol", v.color.tolerance}};
            break;
        case ValueType::Image:
            j["v"] = {{"ref", v.image.refIndex}, {"slot", v.image.captureSlot},
                      {"w", v.image.width}, {"h", v.image.height}, {"label", v.image.label}};
            break;
        case ValueType::ImageResult:
            j["v"] = {{"found", v.imageResult.found},
                      {"sim", v.imageResult.similarity},
                      {"coord", {v.imageResult.coordinates.x, v.imageResult.coordinates.y}},
                      {"reg", {v.imageResult.searchRegion.x, v.imageResult.searchRegion.y,
                               v.imageResult.searchRegion.w, v.imageResult.searchRegion.h}},
                      {"box", {v.imageResult.matchedBox.x, v.imageResult.matchedBox.y,
                               v.imageResult.matchedBox.w, v.imageResult.matchedBox.h}},
                      {"durMs", v.imageResult.searchDurationMs},
                      {"count", v.imageResult.matchCount}};
            break;
        case ValueType::Array: {
            json arr = json::array();
            for (const auto& item : v.arr) arr.push_back(valueToJson(item, depth + 1));
            j["v"] = std::move(arr);
            break;
        }
        case ValueType::Object: {
            json obj = json::object();
            for (const auto& [k, item] : v.obj) obj[k] = valueToJson(item, depth + 1);
            j["v"] = std::move(obj);
            break;
        }
    }
    return j;
}

Value valueFromJson(const json& j, int depth = 0) {
    if (depth > 8 || !j.is_object()) return Value::makeBool(false);
    const ValueType t = parseValueType(j.value("t", std::string("Boolean"))).value_or(ValueType::Boolean);
    Value out = Value::defaultFor(t);
    if (!j.contains("v")) return out;
    const json& v = j["v"];
    switch (t) {
        case ValueType::Boolean: if (v.is_boolean()) out.b = v.get<bool>(); break;
        case ValueType::Integer: if (v.is_number_integer()) out.i = v.get<int64_t>(); break;
        case ValueType::Float: if (v.is_number()) out.f = v.get<double>(); break;
        case ValueType::String:
        case ValueType::Any: if (v.is_string()) out.s = v.get<std::string>(); break;
        case ValueType::Coordinate:
        case ValueType::Vector2:
            if (v.is_array() && v.size() == 2) {
                out.v = {v[0].get<float>(), v[1].get<float>()};
                if (t == ValueType::Coordinate)
                    out.v = {std::clamp(out.v.x, 0.0f, 1.0f), std::clamp(out.v.y, 0.0f, 1.0f)};
            }
            break;
        case ValueType::Color:
            if (v.is_object()) {
                out.color.r = static_cast<uint8_t>(std::clamp(v.value("r", 255), 0, 255));
                out.color.g = static_cast<uint8_t>(std::clamp(v.value("g", 0), 0, 255));
                out.color.b = static_cast<uint8_t>(std::clamp(v.value("b", 0), 0, 255));
                out.color.a = static_cast<uint8_t>(std::clamp(v.value("a", 255), 0, 255));
                out.color.tolerance = std::clamp(v.value("tol", 12.0f), 0.0f, 100.0f);
            }
            break;
        case ValueType::Image:
            if (v.is_object()) {
                out.image.refIndex = v.value("ref", -1);
                out.image.captureSlot = v.value("slot", -1);
                out.image.width = v.value("w", 0u);
                out.image.height = v.value("h", 0u);
                out.image.label = v.value("label", std::string());
            }
            break;
        case ValueType::ImageResult:
            if (v.is_object()) {
                out.imageResult.found = v.value("found", false);
                out.imageResult.similarity = v.value("sim", 0.0f);
                if (v.contains("coord") && v["coord"].is_array() && v["coord"].size() == 2)
                    out.imageResult.coordinates = {v["coord"][0].get<float>(), v["coord"][1].get<float>()};
                if (v.contains("reg") && v["reg"].is_array() && v["reg"].size() == 4)
                    out.imageResult.searchRegion = {v["reg"][0].get<float>(), v["reg"][1].get<float>(),
                                                    v["reg"][2].get<float>(), v["reg"][3].get<float>()};
                if (v.contains("box") && v["box"].is_array() && v["box"].size() == 4)
                    out.imageResult.matchedBox = {v["box"][0].get<float>(), v["box"][1].get<float>(),
                                                  v["box"][2].get<float>(), v["box"][3].get<float>()};
                out.imageResult.searchDurationMs = v.value("durMs", 0.0f);
                out.imageResult.matchCount = v.value("count", 0);
            }
            break;
        case ValueType::Array:
            if (v.is_array())
                for (const auto& item : v) out.arr.push_back(valueFromJson(item, depth + 1));
            break;
        case ValueType::Object:
            if (v.is_object())
                for (auto it = v.begin(); it != v.end(); ++it)
                    out.obj[it.key()] = valueFromJson(it.value(), depth + 1);
            break;
    }
    return out;
}

json portToJson(const Port& p) {
    return {{"id", p.id},
            {"key", p.key},
            {"name", p.name},
            {"kind", p.kind == PortKind::Execution ? "exec" : "data"},
            {"dataType", valueTypeName(p.dataType)},
            {"dir", p.dir == PortDir::Input ? "in" : "out"},
            {"default", valueToJson(p.defaultValue)}};
}

Port portFromJson(const json& j) {
    Port p;
    p.id = j.value("id", 0u);
    p.key = j.value("key", std::string());
    p.name = j.value("name", std::string());
    p.kind = j.value("kind", std::string("exec")) == "data" ? PortKind::Data : PortKind::Execution;
    p.dataType = parseValueType(j.value("dataType", std::string("Boolean"))).value_or(ValueType::Boolean);
    p.dir = j.value("dir", std::string("in")) == "out" ? PortDir::Output : PortDir::Input;
    if (j.contains("default")) p.defaultValue = valueFromJson(j["default"]);
    return p;
}

void migrateGraphJson(json& j, std::vector<std::string>* notes) {
    int ver = j.value("version", 0);
    if (ver == 0) {
        // v0 drafts used "links" instead of "connections" and lacked canvas view state.
        if (j.contains("links") && !j.contains("connections")) {
            j["connections"] = j["links"];
            j.erase("links");
            if (notes) notes->push_back("Migrated v0 'links' array to v1 'connections'.");
        }
        if (!j.contains("view")) {
            j["view"] = {{"pan", {0.0f, 0.0f}}, {"zoom", 1.0f}, {"wireStyle", 0},
                         {"showGrid", true}, {"showMinimap", true}, {"snapToGrid", true}, {"gridSize", 24.0f}};
        }
        j["version"] = 1;
        if (notes) notes->push_back("Upgraded graph schema version 0 -> 1.");
    }
}

json graphToJsonObj(const AutomationGraph& g) {
    json nodesArr = json::array();
    for (const auto& n : g.nodes) {
        json ins = json::array(), outs = json::array(), props = json::object();
        for (const auto& p : n.inputs) ins.push_back(portToJson(p));
        for (const auto& p : n.outputs) outs.push_back(portToJson(p));
        for (const auto& [k, val] : n.props) props[k] = valueToJson(val);
        json no{{"id", n.id},
                {"type", n.type == NodeType::CustomPlugin ? n.customTypeKey : std::string(nodeTypeKey(n.type))},
                {"custom", n.type == NodeType::CustomPlugin},
                {"title", n.title},
                {"pos", {n.pos.x, n.pos.y}},
                {"size", {n.size.x, n.size.y}},
                {"enabled", n.enabled},
                {"breakpoint", n.breakpoint},
                {"collapsed", n.collapsed},
                {"inputs", std::move(ins)},
                {"outputs", std::move(outs)},
                {"props", std::move(props)}};
        if (!n.comment.empty()) no["comment"] = n.comment;
        if (!n.subgraphId.empty()) no["subgraphId"] = n.subgraphId;
        if (n.triggerKey) no["triggerKey"] = n.triggerKey;
        if (n.triggerKey2) no["triggerKey2"] = n.triggerKey2;
        nodesArr.push_back(std::move(no));
    }

    json connsArr = json::array();
    for (const auto& c : g.connections) {
        connsArr.push_back({{"id", c.id},
                            {"fromNode", c.fromNode},
                            {"fromPort", c.fromPort},
                            {"toNode", c.toNode},
                            {"toPort", c.toPort},
                            {"kind", c.kind == PortKind::Execution ? "exec" : "data"}});
    }

    json varsArr = json::array();
    for (const auto& v : g.variables) {
        varsArr.push_back({{"name", v.name},
                           {"type", valueTypeName(v.type)},
                           {"initial", valueToJson(v.initialValue)},
                           {"description", v.description}});
    }

    json groupsArr = json::array();
    for (const auto& gr : g.groups) {
        groupsArr.push_back({{"id", gr.id},
                             {"title", gr.title},
                             {"rect", {gr.rect.x, gr.rect.y, gr.rect.w, gr.rect.h}},
                             {"color", gr.colorRgba},
                             {"collapsed", gr.collapsed},
                             {"nodeIds", gr.nodeIds},
                             {"comment", gr.comment}});
    }

    json notesArr = json::array();
    for (const auto& nt : g.notes) {
        notesArr.push_back({{"id", nt.id},
                            {"title", nt.title},
                            {"text", nt.text},
                            {"pos", {nt.pos.x, nt.pos.y}},
                            {"size", {nt.size.x, nt.size.y}},
                            {"color", nt.colorRgba}});
    }

    json subsArr = json::array();
    for (const auto& sub : g.subgraphs) subsArr.push_back(graphToJsonObj(sub));

    return {{"id", g.id.empty() ? generateGraphId() : g.id},
            {"name", g.name},
            {"version", g.version},
            {"nextId", g.nextId},
            {"nodes", std::move(nodesArr)},
            {"connections", std::move(connsArr)},
            {"variables", std::move(varsArr)},
            {"groups", std::move(groupsArr)},
            {"notes", std::move(notesArr)},
            {"subgraphs", std::move(subsArr)},
            {"view",
             {{"pan", {g.view.pan.x, g.view.pan.y}},
              {"zoom", g.view.zoom},
              {"wireStyle", static_cast<int>(g.view.wireStyle)},
              {"showGrid", g.view.showGrid},
              {"showMinimap", g.view.showMinimap},
              {"snapToGrid", g.view.snapToGrid},
              {"gridSize", g.view.gridSize}}},
            {"metadata",
             {{"description", g.metadata.description},
              {"author", g.metadata.author},
              {"createdAt", g.metadata.createdAt},
              {"updatedAt", g.metadata.updatedAt},
              {"triggerKey", g.metadata.triggerKey},
              {"triggerKey2", g.metadata.triggerKey2},
              {"maxDurationMs", g.metadata.maxDurationMs},
              {"maxStepsBudget", g.metadata.maxStepsBudget},
              {"sourceLegacyMacro", g.metadata.sourceLegacyMacro},
              {"conversionNotes", g.metadata.conversionNotes}}}};
}

bool graphFromJsonObj(json j, AutomationGraph& out, std::string& err, std::vector<std::string>* migrationNotes,
                      int depth = 0) {
    if (depth > kMaxGraphDepth) {
        err = "subgraph nesting too deep";
        return false;
    }
    if (!j.is_object()) {
        err = "graph JSON must be an object";
        return false;
    }
    migrateGraphJson(j, migrationNotes);
    const int ver = j.value("version", 1);
    if (ver > kGraphSchemaVersion) {
        err = "graph schema version " + std::to_string(ver) + " is newer than supported (" +
              std::to_string(kGraphSchemaVersion) + ")";
        return false;
    }

    AutomationGraph g;
    g.id = j.value("id", generateGraphId());
    g.name = j.value("name", std::string("Untitled Graph"));
    g.version = ver;
    g.nextId = std::max(1u, j.value("nextId", 1u));
    uint32_t maxSeenId = 0;

    for (const auto& no : j.value("nodes", json::array())) {
        GraphNode n;
        n.id = no.value("id", 0u);
        maxSeenId = std::max(maxSeenId, n.id);
        const std::string tkey = no.value("type", std::string("flow.start"));
        const bool isCustom = no.value("custom", false);
        if (isCustom) {
            n.type = NodeType::CustomPlugin;
            n.customTypeKey = tkey;
        } else if (auto parsed = parseNodeTypeKey(tkey)) {
            n.type = *parsed;
        } else {
            err = "unknown node type: " + tkey;
            return false;
        }
        n.title = no.value("title", std::string());
        if (no.contains("pos") && no["pos"].is_array() && no["pos"].size() == 2)
            n.pos = {no["pos"][0].get<float>(), no["pos"][1].get<float>()};
        if (no.contains("size") && no["size"].is_array() && no["size"].size() == 2)
            n.size = {no["size"][0].get<float>(), no["size"][1].get<float>()};
        n.enabled = no.value("enabled", true);
        n.breakpoint = no.value("breakpoint", false);
        n.collapsed = no.value("collapsed", false);
        n.comment = no.value("comment", std::string());
        n.subgraphId = no.value("subgraphId", std::string());
        n.triggerKey = std::clamp(no.value("triggerKey", 0), 0, 0xFE);
        n.triggerKey2 = std::clamp(no.value("triggerKey2", 0), 0, 0xFE);

        for (const auto& po : no.value("inputs", json::array())) {
            Port p = portFromJson(po);
            p.dir = PortDir::Input;
            maxSeenId = std::max(maxSeenId, p.id);
            n.inputs.push_back(std::move(p));
        }
        for (const auto& po : no.value("outputs", json::array())) {
            Port p = portFromJson(po);
            p.dir = PortDir::Output;
            maxSeenId = std::max(maxSeenId, p.id);
            n.outputs.push_back(std::move(p));
        }

        // Seed defaults from spec first so newly added properties on older saved files get safe defaults.
        if (const NodeTypeSpec* spec = findNodeSpec(n.type, n.customTypeKey)) {
            if (n.title.empty()) n.title = spec->displayName;
            for (const auto& pr : spec->properties) n.props[pr.key] = pr.defaultValue;
        }
        if (no.contains("props") && no["props"].is_object()) {
            for (auto it = no["props"].begin(); it != no["props"].end(); ++it)
                n.props[it.key()] = valueFromJson(it.value());
        }
        g.nodes.push_back(std::move(n));
    }

    for (const auto& co : j.value("connections", json::array())) {
        GraphConnection c;
        c.id = co.value("id", 0u);
        maxSeenId = std::max(maxSeenId, c.id);
        c.fromNode = co.value("fromNode", 0u);
        c.fromPort = co.value("fromPort", 0u);
        c.toNode = co.value("toNode", 0u);
        c.toPort = co.value("toPort", 0u);
        c.kind = co.value("kind", std::string("exec")) == "data" ? PortKind::Data : PortKind::Execution;
        g.connections.push_back(c);
    }

    for (const auto& vo : j.value("variables", json::array())) {
        GraphVariable v;
        v.name = vo.value("name", std::string());
        v.type = parseValueType(vo.value("type", std::string("Integer"))).value_or(ValueType::Integer);
        v.initialValue = vo.contains("initial") ? valueFromJson(vo["initial"]) : Value::defaultFor(v.type);
        v.description = vo.value("description", std::string());
        g.variables.push_back(std::move(v));
    }

    for (const auto& go : j.value("groups", json::array())) {
        GraphGroup gr;
        gr.id = go.value("id", 0u);
        maxSeenId = std::max(maxSeenId, gr.id);
        gr.title = go.value("title", std::string("Group"));
        if (go.contains("rect") && go["rect"].is_array() && go["rect"].size() == 4)
            gr.rect = {go["rect"][0].get<float>(), go["rect"][1].get<float>(),
                       go["rect"][2].get<float>(), go["rect"][3].get<float>()};
        gr.colorRgba = go.value("color", 0x3B82F640u);
        gr.collapsed = go.value("collapsed", false);
        gr.nodeIds = go.value("nodeIds", std::vector<uint32_t>{});
        gr.comment = go.value("comment", std::string());
        g.groups.push_back(std::move(gr));
    }

    for (const auto& nto : j.value("notes", json::array())) {
        GraphNote nt;
        nt.id = nto.value("id", 0u);
        maxSeenId = std::max(maxSeenId, nt.id);
        nt.title = nto.value("title", std::string("Note"));
        nt.text = nto.value("text", std::string());
        if (nto.contains("pos") && nto["pos"].is_array() && nto["pos"].size() == 2)
            nt.pos = {nto["pos"][0].get<float>(), nto["pos"][1].get<float>()};
        if (nto.contains("size") && nto["size"].is_array() && nto["size"].size() == 2)
            nt.size = {nto["size"][0].get<float>(), nto["size"][1].get<float>()};
        nt.colorRgba = nto.value("color", 0xF59E0B33u);
        g.notes.push_back(std::move(nt));
    }

    for (const auto& so : j.value("subgraphs", json::array())) {
        AutomationGraph sub;
        if (!graphFromJsonObj(so, sub, err, migrationNotes, depth + 1)) return false;
        g.subgraphs.push_back(std::move(sub));
    }

    if (j.contains("view") && j["view"].is_object()) {
        const auto& vw = j["view"];
        if (vw.contains("pan") && vw["pan"].is_array() && vw["pan"].size() == 2)
            g.view.pan = {vw["pan"][0].get<float>(), vw["pan"][1].get<float>()};
        g.view.zoom = std::clamp(vw.value("zoom", 1.0f), 0.1f, 4.0f);
        g.view.wireStyle = static_cast<WireStyle>(std::clamp(vw.value("wireStyle", 0), 0, 2));
        g.view.showGrid = vw.value("showGrid", true);
        g.view.showMinimap = vw.value("showMinimap", true);
        g.view.snapToGrid = vw.value("snapToGrid", true);
        g.view.gridSize = std::clamp(vw.value("gridSize", 24.0f), 8.0f, 128.0f);
    }

    if (j.contains("metadata") && j["metadata"].is_object()) {
        const auto& md = j["metadata"];
        g.metadata.description = md.value("description", std::string());
        g.metadata.author = md.value("author", std::string());
        g.metadata.createdAt = md.value("createdAt", std::string());
        g.metadata.updatedAt = md.value("updatedAt", std::string());
        g.metadata.triggerKey = std::clamp(md.value("triggerKey", 0), 0, 0xFE);
        g.metadata.triggerKey2 = std::clamp(md.value("triggerKey2", 0), 0, 0xFE);
        g.metadata.maxDurationMs = std::clamp(md.value("maxDurationMs", 60000), 50, 3600000);
        g.metadata.maxStepsBudget = std::clamp(md.value("maxStepsBudget", 10000), 10, 1000000);
        g.metadata.sourceLegacyMacro = md.value("sourceLegacyMacro", std::string());
        g.metadata.conversionNotes = md.value("conversionNotes", std::vector<std::string>{});
    }

    g.nextId = std::max(g.nextId, maxSeenId + 1);

    if (const auto problems = validateGraph(g); !problems.empty()) {
        err = problems.front();
        return false;
    }
    out = std::move(g);
    return true;
}

}  // namespace

std::string graphToJson(const AutomationGraph& g, int indent) {
    return graphToJsonObj(g).dump(indent);
}

bool graphFromJson(const std::string& text, AutomationGraph& out, std::string& err,
                   std::vector<std::string>* migrationNotes) {
    try {
        json j = json::parse(text);
        return graphFromJsonObj(std::move(j), out, err, migrationNotes);
    } catch (const std::exception& e) {
        err = std::string("invalid graph JSON: ") + e.what();
        return false;
    }
}

bool saveGraphFile(const std::string& path, const AutomationGraph& g, std::string& err) {
    if (const auto problems = validateGraph(g); !problems.empty()) {
        err = problems.front();
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open " + tmp.string() + " for writing";
            return false;
        }
        f << graphToJson(g, 2);
        if (!f) {
            err = "failed writing " + tmp.string();
            return false;
        }
    }
    if (fs::exists(target, ec)) {
        fs::path bak = target;
        bak += ".bak";
        fs::copy_file(target, bak, fs::copy_options::overwrite_existing, ec);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        err = "atomic rename failed: " + ec.message();
        return false;
    }
    return true;
}

bool loadGraphFile(const std::string& path, AutomationGraph& out, std::string& err,
                   std::vector<std::string>* migrationNotes) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return graphFromJson(ss.str(), out, err, migrationNotes);
}

}  // namespace wasd::studio
