// NEXPLAY Automation Studio — Graph Data Model, Port/Type System & Serialization.
// Pure C++20 logic inside wasd_core (no ImGui / OS / D3D dependencies), unit-tested.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/coords.h"

namespace wasd::studio {

inline constexpr int kGraphSchemaVersion = 1;
inline constexpr int kMaxGraphNodes = 2000;
inline constexpr int kMaxGraphDepth = 16;

// ---------------------------------------------------------------- Data Types

enum class ValueType : uint8_t {
    Boolean = 0,
    Integer,
    Float,
    String,
    Coordinate,   // normalized screen point [0, 1] x [0, 1]
    Vector2,      // general 2D vector / offset
    Color,        // RGBA8 + tolerance
    Image,        // reference image index or captured frame handle
    ImageResult,  // structured output of Find Image / Wait For Image
    Array,        // ordered list of Values
    Object,       // key-value map of Values
    Any           // polymorphic wildcard port (Equal, IsEmpty, Get/Set Variable)
};

const char* valueTypeName(ValueType t);
std::optional<ValueType> parseValueType(const std::string& s);

struct ColorVal {
    uint8_t r = 255, g = 0, b = 0, a = 255;
    float tolerance = 12.0f;
    bool operator==(const ColorVal&) const = default;
};

struct ImageHandle {
    int refIndex = -1;       // >= 0 refers to Automation::images[refIndex]
    int captureSlot = -1;    // >= 0 refers to runtime captured screenshot slot
    uint32_t width = 0;
    uint32_t height = 0;
    std::string label;
    bool operator==(const ImageHandle&) const = default;
};

struct ImageResultVal {
    bool found = false;
    float similarity = 0.0f;      // [0, 1] or [0, 100] normalized to [0, 1]
    Vec2 coordinates{0.5f, 0.5f}; // normalized center of match
    Rect searchRegion{0.0f, 0.0f, 1.0f, 1.0f};
    Rect matchedBox{};
    float searchDurationMs = 0.0f;
    int matchCount = 0;
    bool operator==(const ImageResultVal&) const = default;
};

// Result of an arbitrary region color scan (Find Color without a saved zone).
struct ColorScanResult {
    bool found = false;
    Vec2 center{0.5f, 0.5f};       // normalized centroid of matched pixels
    ColorVal color{};              // average matched color
    float similarity = 0.0f;       // [0..1] perceptual closeness of the best area
    bool operator==(const ColorScanResult&) const = default;
};

// Result of an object-detection scan (YOLO/ONNX). Coordinates are normalized
// [0,1]; `className` is the model label of the best detection.
struct ObjectScanResult {
    bool found = false;
    Vec2 center{0.5f, 0.5f};       // normalized center of best detection box
    float confidence = 0.0f;       // [0,1] best detection score
    std::string className;         // label of the best detection
    int matchCount = 0;            // detections above threshold (after NMS)
    Rect bestBox{};                // normalized rect of best detection
    bool operator==(const ObjectScanResult&) const = default;
};

struct Value {
    ValueType type = ValueType::Boolean;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    Vec2 v{0.0f, 0.0f};  // used by Coordinate and Vector2
    ColorVal color{};
    ImageHandle image{};
    ImageResultVal imageResult{};
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    static Value makeBool(bool v);
    static Value makeInt(int64_t v);
    static Value makeFloat(double v);
    static Value makeString(std::string v);
    static Value makeCoord(Vec2 c);
    static Value makeVec2(Vec2 v);
    static Value makeColor(uint8_t r, uint8_t g, uint8_t b, float tolerance = 12.0f);
    static Value makeImage(int refIndex, std::string label = {});
    static Value makeImageResult(const ImageResultVal& r);
    static Value makeArray(std::vector<Value> items = {});
    static Value makeObject(std::map<std::string, Value> fields = {});
    static Value defaultFor(ValueType t);

    bool asBool() const;
    int64_t asInt() const;
    double asFloat() const;
    std::string asString() const;
    Vec2 asCoord() const;
    Vec2 asVec2() const;
    bool isEmpty() const;

    bool operator==(const Value&) const = default;
};

// ---------------------------------------------------------------- Ports & Wires

enum class PortKind : uint8_t {
    Execution = 0,  // controls execution sequence
    Data = 1        // carries typed Value between nodes
};

enum class PortDir : uint8_t {
    Input = 0,
    Output = 1
};

struct Port {
    uint32_t id = 0;          // unique within the owning AutomationGraph
    std::string key;          // stable semantic key within the node ("in", "out", "found", "x"...)
    std::string name;         // human-readable label (localized at UI render time via i18n)
    PortKind kind = PortKind::Execution;
    ValueType dataType = ValueType::Boolean;  // meaningful when kind == PortKind::Data
    PortDir dir = PortDir::Input;
    Value defaultValue{};     // inline fallback when an Input Data port is unconnected
    bool operator==(const Port&) const = default;
};

enum class WireStyle : uint8_t {
    Curved = 0,   // smooth cubic Bezier
    Rounded = 1,  // orthogonal rounded-corner metro routing
    Straight = 2  // direct segment
};

struct GraphConnection {
    uint32_t id = 0;
    uint32_t fromNode = 0;  // always Output side
    uint32_t fromPort = 0;
    uint32_t toNode = 0;    // always Input side
    uint32_t toPort = 0;
    PortKind kind = PortKind::Execution;
    bool operator==(const GraphConnection&) const = default;
};

enum class RejectReason : uint8_t {
    None = 0,
    NodeNotFound,
    PortNotFound,
    SelfConnection,
    SameDirection,
    KindMismatch,      // Execution <-> Data forbidden
    TypeMismatch,      // incompatible Data types
    DuplicateWire,
    DataCycle          // cycle in pure/data dependency chain
};

struct ConnectionCheck {
    bool ok = false;
    RejectReason code = RejectReason::None;
    std::string reason;  // clear user-facing explanation (e.g. "Cannot connect Execution port to Data port")
    // Normalized endpoints (Output -> Input) even if the user dragged Input -> Output.
    uint32_t outNode = 0, outPort = 0;
    uint32_t inNode = 0, inPort = 0;
    PortKind kind = PortKind::Execution;
};

bool areDataTypesCompatible(ValueType fromOut, ValueType toIn);
Value coerceValue(const Value& v, ValueType targetType);

// ---------------------------------------------------------------- Node Catalog

enum class NodeCategory : uint8_t {
    FlowControl = 0,
    InputActions,
    ImageRecognition,
    ColorRecognition,
    Logic,
    VariablesMath,
    NexplayCommands
};

const char* categoryName(NodeCategory c);

// Every required node across the 7 specification groups.
// Note: automated aim movement / recoil compensation commands are intentionally
// excluded from the studio node system per specification section 5.G.
enum class NodeType : uint16_t {
    // A. Flow Control
    Start = 0,
    End,
    Delay,
    Loop,
    ForLoop,
    While,
    Break,
    Continue,
    If,
    Else,
    Switch,
    Sequence,
    Function,
    Subgraph,

    // B. Input Actions
    Click,
    DoubleClick,
    LongPress,
    PressDown,
    Release,
    Slide,
    KeyPress,
    KeyRelease,
    Wait,

    // C. Image Recognition
    CaptureScreenshot,
    FindImage,
    ImageFound,
    ImageNotFound,
    CompareSimilarity,
    GetImageCoordinates,
    SelectSearchRegion,
    WaitForImage,
    ImageSearchResult,
    FindObject,

    // D. Color Recognition
    GetPixelColor,
    FindColor,
    CompareColor,
    ColorFound,
    ColorNotFound,
    ColorSimilarity,

    // E. Logic
    LogicAnd,
    LogicOr,
    LogicNot,
    Equal,
    NotEqual,
    GreaterThan,
    LessThan,
    IsEmpty,

    // F. Variables & Math
    CreateVariable,
    SetVariable,
    GetVariable,
    MathAdd,
    MathSubtract,
    MathMultiply,
    MathDivide,
    MathClamp,
    MathMin,
    MathMax,
    RandomValue,

    // G. Existing Compatible NEXPLAY Commands
    RunMacro,
    CheckColorZone,
    StopExecution,
    JumpTarget,   // preserves legacy JumpStep targets losslessly when needed
    CommentNode,

    CustomPlugin  // extensible custom node type identified by GraphNode::customTypeKey
};

const char* nodeTypeKey(NodeType t);
std::optional<NodeType> parseNodeTypeKey(const std::string& key);

struct PortSpec {
    std::string key;
    std::string name;
    PortKind kind = PortKind::Execution;
    ValueType dataType = ValueType::Boolean;
    PortDir dir = PortDir::Input;
    Value defaultValue{};
};

struct PropertySpec {
    std::string key;
    std::string label;
    Value defaultValue;
    bool advanced = false;  // Progressive Disclosure: hidden under "Advanced Settings" until expanded
    double minVal = 0.0;
    double maxVal = 0.0;
    std::string tooltip;
};

struct NodeTypeSpec {
    NodeType type = NodeType::Start;
    std::string typeKey;     // unique serialization key
    std::string displayName; // English UI key (passed through i18n::tr)
    NodeCategory category = NodeCategory::FlowControl;
    std::string icon;        // Segoe MDL2 glyph or compact symbol
    std::string summary;     // concise tooltip description
    std::string example;     // short usage hint for onboarding/tooltips
    bool pureData = false;   // true for nodes with no Execution ports (evaluated on demand)
    std::vector<PortSpec> ports;
    std::vector<PropertySpec> properties;
};

const std::vector<NodeTypeSpec>& allNodeSpecs();
const NodeTypeSpec* findNodeSpec(NodeType t, const std::string& customKey = {});
// Extensibility hook: new node types can be registered at startup without rewriting the engine.
void registerCustomNodeSpec(NodeTypeSpec spec);

// ---------------------------------------------------------------- Graph Elements

struct GraphNode {
    uint32_t id = 0;
    NodeType type = NodeType::Start;
    std::string customTypeKey;  // when type == NodeType::CustomPlugin
    std::string title;          // user renameable; empty = default displayName
    Vec2 pos{0.0f, 0.0f};       // canvas-space coordinates (logical, independent of RTL)
    Vec2 size{220.0f, 0.0f};    // cached/optional layout hint (height 0 = auto from ports)
    bool enabled = true;
    bool breakpoint = false;    // Debug Mode breakpoint
    bool collapsed = false;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::map<std::string, Value> props;  // node-specific inspector properties
    std::string comment;                 // per-node annotation badge
    std::string subgraphId;              // target graph ID for Function / Subgraph nodes
    // Start nodes only: per-entry hotkey — lets one graph host several
    // independent flows ("macros") each launched by its own key. The legacy
    // GraphMetadata::triggerKey still applies to the FIRST Start as a fallback.
    int triggerKey = 0;
    int triggerKey2 = 0;                 // optional combo key (hold key1, tap key2)

    const Port* findPort(uint32_t portId) const;
    Port* findPort(uint32_t portId);
    const Port* findInputByKey(const std::string& key) const;
    const Port* findOutputByKey(const std::string& key) const;
    Value getProp(const std::string& key, const Value& fallback = {}) const;
    void setProp(const std::string& key, Value v);

    bool operator==(const GraphNode&) const = default;
};

struct GraphVariable {
    std::string name;
    ValueType type = ValueType::Integer;
    Value initialValue = Value::makeInt(0);
    std::string description;
    bool operator==(const GraphVariable&) const = default;
};

// Visual Frame / Group container on the canvas (collapsible).
struct GraphGroup {
    uint32_t id = 0;
    std::string title = "Group";
    Rect rect{0.0f, 0.0f, 420.0f, 260.0f};
    uint32_t colorRgba = 0x3B82F640u;
    bool collapsed = false;
    std::vector<uint32_t> nodeIds;
    std::string comment;
    bool operator==(const GraphGroup&) const = default;
};

// Free-floating sticky note / documentation block on the canvas.
struct GraphNote {
    uint32_t id = 0;
    std::string title = "Note";
    std::string text;
    Vec2 pos{0.0f, 0.0f};
    Vec2 size{240.0f, 110.0f};
    uint32_t colorRgba = 0xF59E0B33u;
    bool operator==(const GraphNote&) const = default;
};

struct CanvasViewState {
    Vec2 pan{0.0f, 0.0f};
    float zoom = 1.0f;
    WireStyle wireStyle = WireStyle::Curved;
    bool showGrid = true;
    bool showMinimap = true;
    bool snapToGrid = true;
    float gridSize = 24.0f;
    bool operator==(const CanvasViewState&) const = default;
};

struct GraphMetadata {
    std::string description;
    std::string author;
    std::string createdAt;
    std::string updatedAt;
    int triggerKey = 0;          // optional hotkey to launch this graph directly
    int triggerKey2 = 0;         // optional combo key
    int maxDurationMs = 60000;   // overall execution timeout
    int maxStepsBudget = 10000;  // unintended infinite-loop guard (configurable)
    std::string sourceLegacyMacro;  // non-empty if created by Legacy Macro Converter
    std::vector<std::string> conversionNotes;
    bool operator==(const GraphMetadata&) const = default;
};

// Copy/Paste payload preserving internal connections between copied nodes.
struct GraphClipboard {
    std::vector<GraphNode> nodes;
    std::vector<GraphConnection> internalConnections;
    std::vector<GraphNote> notes;
    bool empty() const { return nodes.empty() && notes.empty(); }
};

struct AutomationGraph {
    std::string id;
    std::string name = "New Graph";
    int version = kGraphSchemaVersion;
    uint32_t nextId = 1;

    std::vector<GraphNode> nodes;
    std::vector<GraphConnection> connections;
    std::vector<GraphVariable> variables;
    std::vector<GraphGroup> groups;
    std::vector<GraphNote> notes;
    std::vector<AutomationGraph> subgraphs;  // embedded reusable Functions / Subgraphs

    CanvasViewState view{};
    GraphMetadata metadata{};

    // ---- Node & Port queries ----
    uint32_t allocateId() { return nextId++; }
    GraphNode* findNode(uint32_t nodeId);
    const GraphNode* findNode(uint32_t nodeId) const;
    bool findPortOwner(uint32_t portId, const GraphNode*& outNode, const Port*& outPort) const;

    // ---- Editing operations ----
    GraphNode& addNode(NodeType type, Vec2 pos, const std::string& customKey = {});
    bool removeNode(uint32_t nodeId);
    ConnectionCheck canConnect(uint32_t nodeA, uint32_t portA, uint32_t nodeB, uint32_t portB) const;
    // Connects two ports (auto-normalizing Output->Input). When replaceExisting is true,
    // an existing wire on a single-wire slot (Execution Output or Data Input) is replaced.
    ConnectionCheck connect(uint32_t nodeA, uint32_t portA, uint32_t nodeB, uint32_t portB,
                            uint32_t* outConnId = nullptr, bool replaceExisting = true);
    ConnectionCheck connectByKey(uint32_t outNodeId, const std::string& outPortKey,
                                 uint32_t inNodeId, const std::string& inPortKey,
                                 uint32_t* outConnId = nullptr, bool replaceExisting = true);
    bool disconnect(uint32_t connectionId);
    int disconnectPort(uint32_t portId);

    // Copy / Paste / Duplicate
    GraphClipboard copySelection(const std::vector<uint32_t>& nodeIds,
                                 const std::vector<uint32_t>& noteIds = {}) const;
    std::vector<uint32_t> pasteClipboard(const GraphClipboard& clip, Vec2 offset = {36.0f, 36.0f});

    // Groups & Notes
    GraphGroup& addGroup(const std::string& title, Rect rect, const std::vector<uint32_t>& memberNodes = {});
    bool removeGroup(uint32_t groupId);
    GraphNote& addNote(const std::string& title, const std::string& text, Vec2 pos);
    bool removeNote(uint32_t noteId);

    // Variables
    bool upsertVariable(GraphVariable var);
    bool removeVariable(const std::string& varName);
    const GraphVariable* findVariable(const std::string& varName) const;

    // Auto-layout (layered left-to-right topological Sugiyama-lite arrangement)
    void autoArrange(float startX = 80.0f, float startY = 100.0f, float colSpacing = 290.0f, float rowSpacing = 130.0f);

    bool operator==(const AutomationGraph&) const = default;
};

// Undo / Redo snapshot stack dedicated to AutomationGraph editing.
class GraphHistory {
public:
    explicit GraphHistory(std::size_t limit = 100) : limit_(limit) {}
    void reset(const AutomationGraph& g);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool commit(const AutomationGraph& g);
    bool undo(AutomationGraph& out);
    bool redo(AutomationGraph& out);
    const AutomationGraph& base() const { return base_; }

private:
    std::size_t limit_ = 100;
    AutomationGraph base_{};
    std::vector<AutomationGraph> undo_, redo_;
};

// Starter & template factories
std::string generateGraphId();
AutomationGraph makeStarterGraph(const std::string& name = "New Graph");

enum class GraphTemplateKind : uint8_t {
    BasicSequence = 0,
    ConditionalFlow,
    ImageRecognitionExample,
    ColorDetectionExample,
    LoopExample,
    VariablesExample
};
AutomationGraph makeTemplateGraph(GraphTemplateKind kind);

// Validation, Serialization & Version Migration
std::vector<std::string> validateGraph(const AutomationGraph& g, int depth = 0);
std::string graphToJson(const AutomationGraph& g, int indent = 2);
bool graphFromJson(const std::string& text, AutomationGraph& out, std::string& err,
                   std::vector<std::string>* migrationNotes = nullptr);
bool saveGraphFile(const std::string& path, const AutomationGraph& g, std::string& err);
bool loadGraphFile(const std::string& path, AutomationGraph& out, std::string& err,
                   std::vector<std::string>* migrationNotes = nullptr);

}  // namespace wasd::studio
